/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <csyn/csyn.h>
#include <csyn/csyn_codec.h>
#include <csyn/csyn_zros.h>

#include <errno.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zros/private/zros_topic_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_topic.h>

LOG_MODULE_REGISTER(csyn_zros, LOG_LEVEL_INF);

/* Optional vehicle-owned storage. Undefined weak symbols resolve to NULL, so
 * the bridge has no link-time or runtime dependency on undeclared topics. */
#define CSYN_ZROS_WEAK_TOPIC(_name) extern struct zros_topic topic_##_name __attribute__((weak))
CSYN_ZROS_WEAK_TOPIC(manual_control);
CSYN_ZROS_WEAK_TOPIC(mocap);
CSYN_ZROS_WEAK_TOPIC(inertial_sample);
CSYN_ZROS_WEAK_TOPIC(odometry);
CSYN_ZROS_WEAK_TOPIC(pwm_signal_outputs);
CSYN_ZROS_WEAK_TOPIC(vehicle_health);
CSYN_ZROS_WEAK_TOPIC(attitude_estimate);
CSYN_ZROS_WEAK_TOPIC(attitude_command);
CSYN_ZROS_WEAK_TOPIC(control_loop_metrics);
CSYN_ZROS_WEAK_TOPIC(mission_progress);
CSYN_ZROS_WEAK_TOPIC(local_position_command);
CSYN_ZROS_WEAK_TOPIC(vehicle_command);
CSYN_ZROS_WEAK_TOPIC(navigation_target);

uint32_t csyn_zros_generation(const struct zros_topic *topic)
{
	if (topic == NULL) {
		return 0U;
	}
	return (uint32_t)atomic_get((atomic_t *)&topic->_lockless_generation);
}

static K_THREAD_STACK_DEFINE(g_bridge_stack, 2048);
static struct k_thread g_bridge_thread;
static struct zros_node g_bridge_node;
static struct zros_pub g_manual_control_pub;
static struct zros_pub g_mocap_pub;
static struct zros_pub g_inertial_sample_pub;
static struct zros_pub g_odometry_pub;
static struct csyn_manual_control g_manual_control;
static struct csyn_mocap_rigid_body g_mocap;
static synapse_topic_InertialSampleData_t g_inertial_sample;
static synapse_topic_OdometryData_t g_odometry;

/*
 * Fixed-layout TX topics carry the same struct bytes on both buses, so
 * mirroring zros -> csyn is an identity copy per topic.
 */
struct bridge_tx_map {
	struct zros_topic *zros;
	const char *csyn_key;
	void *msg;
	size_t msg_size;
	struct csyn_topic *csyn;
	uint32_t last_generation;
};

static synapse_topic_PwmSignalOutputsData_t g_pwm_msg;
static synapse_topic_VehicleHealthData_t g_health_msg;
static synapse_topic_AttitudeEstimateData_t g_att_est_msg;
static synapse_topic_AttitudeCommandData_t g_att_cmd_msg;
static synapse_topic_ControlLoopMetricsData_t g_metrics_msg;
static synapse_topic_MissionProgressData_t g_mission_progress_msg;
static synapse_topic_LocalPositionCommandData_t g_local_pos_cmd_msg;
static struct csyn_vehicle_command g_vehicle_cmd_msg;
static synapse_topic_NavigationTargetData_t g_nav_target_msg;

static struct bridge_tx_map g_tx_maps[] = {
	{&topic_pwm_signal_outputs, "pwm", &g_pwm_msg, sizeof(g_pwm_msg)},
	{&topic_vehicle_health, "health", &g_health_msg, sizeof(g_health_msg)},
	{&topic_attitude_estimate, "att", &g_att_est_msg, sizeof(g_att_est_msg)},
	{&topic_attitude_command, "att_sp", &g_att_cmd_msg, sizeof(g_att_cmd_msg)},
	{&topic_control_loop_metrics, "loop", &g_metrics_msg, sizeof(g_metrics_msg)},
	{&topic_mission_progress, "mission", &g_mission_progress_msg,
	 sizeof(g_mission_progress_msg)},
	{&topic_local_position_command, "pos_sp", &g_local_pos_cmd_msg,
	 sizeof(g_local_pos_cmd_msg)},
	{&topic_vehicle_command, "vehicle_command", &g_vehicle_cmd_msg, sizeof(g_vehicle_cmd_msg)},
	{&topic_navigation_target, "nav", &g_nav_target_msg, sizeof(g_nav_target_msg)},
};

static bool copy_csyn_topic(struct csyn_topic *topic, uint8_t *buf, size_t buf_size, size_t *len,
			    uint32_t *last_generation)
{
	uint32_t generation = csyn_topic_generation(topic);

	if (generation == 0U || generation == *last_generation) {
		return false;
	}

	if (!csyn_topic_copy(topic, buf, buf_size, len, NULL)) {
		return false;
	}

	*last_generation = generation;
	return true;
}

static void publish_manual_control_if_updated(struct csyn_topic *topic, uint32_t *last_generation)
{
	uint8_t buf[sizeof(synapse_topic_ManualControlData_t)];
	csyn_rc_channels16_t rc;
	size_t len = 0U;
	bool valid = false;

	if (topic == NULL || !copy_csyn_topic(topic, buf, sizeof(buf), &len, last_generation)) {
		return;
	}

	if (!csyn_decode_manual_control(buf, len, &rc, &valid)) {
		return;
	}

	g_manual_control = (struct csyn_manual_control){
		.rc = rc,
		.valid = valid,
		.stamp_ms = k_uptime_get(),
	};
	(void)zros_pub_update(&g_manual_control_pub);
}

static void publish_mocap_if_updated(struct csyn_topic *topic, uint32_t *last_generation)
{
	uint8_t buf[CONFIG_CSYN_FLATBUFFER_MAX_SIZE];
	size_t len = 0U;
	static bool logged_decode_fail;
	static bool logged_decode_ok;

	if (topic == NULL || !copy_csyn_topic(topic, buf, sizeof(buf), &len, last_generation)) {
		return;
	}

	if (!csyn_decode_mocap_frame(buf, len, CONFIG_CSYN_MOCAP_RIGID_BODY_ID, &g_mocap)) {
		if (!logged_decode_fail) {
			LOG_WRN("mocap decode failed len=%u", (unsigned int)len);
			logged_decode_fail = true;
		}
		return;
	}

	if (!logged_decode_ok) {
		LOG_INF("mocap decoded valid=%d pos=[%.3f %.3f %.3f]", g_mocap.valid ? 1 : 0,
			(double)g_mocap.x, (double)g_mocap.y, (double)g_mocap.z);
		logged_decode_ok = true;
	}
	(void)zros_pub_update(&g_mocap_pub);
}

static void publish_odometry_if_updated(struct csyn_topic *topic, uint32_t *last_generation)
{
	size_t len = 0U;
	static bool logged_size_fail;
	static bool logged_ok;

	if (topic == NULL || !copy_csyn_topic(topic, (uint8_t *)&g_odometry, sizeof(g_odometry),
					      &len, last_generation)) {
		return;
	}

	if (len != sizeof(g_odometry)) {
		if (!logged_size_fail) {
			LOG_WRN("odometry size mismatch len=%u expected=%u", (unsigned int)len,
				(unsigned int)sizeof(g_odometry));
			logged_size_fail = true;
		}
		return;
	}

	if (!logged_ok) {
		LOG_INF("odometry received pos=[%.3f %.3f %.3f]",
			(double)g_odometry.pose.position_enu_m.x,
			(double)g_odometry.pose.position_enu_m.y,
			(double)g_odometry.pose.position_enu_m.z);
		logged_ok = true;
	}
	(void)zros_pub_update(&g_odometry_pub);

	if (IS_ENABLED(CONFIG_CSYN_MOCAP_SOURCE_ODOMETRY) && &topic_mocap != NULL) {
		const uint32_t pose_valid = synapse_topic_OdometryFlags_PositionValid |
					    synapse_topic_OdometryFlags_AttitudeValid;
		uint32_t flags = g_odometry.flags;

		g_mocap = (struct csyn_mocap_rigid_body){
			.id = 0,
			.x = g_odometry.pose.position_enu_m.x,
			.y = g_odometry.pose.position_enu_m.y,
			.z = g_odometry.pose.position_enu_m.z,
			.qw = g_odometry.pose.attitude.w,
			.qx = g_odometry.pose.attitude.x,
			.qy = g_odometry.pose.attitude.y,
			.qz = g_odometry.pose.attitude.z,
			.valid = (flags & pose_valid) == pose_valid &&
				 (flags & synapse_topic_OdometryFlags_Lost) == 0U,
		};
		(void)zros_pub_update(&g_mocap_pub);
	}
}

static void publish_inertial_sample_if_updated(struct csyn_topic *topic, uint32_t *last_generation)
{
	size_t len = 0U;

	if (topic == NULL ||
	    !copy_csyn_topic(topic, (uint8_t *)&g_inertial_sample, sizeof(g_inertial_sample), &len,
			     last_generation) ||
	    len != sizeof(g_inertial_sample)) {
		return;
	}
	(void)zros_pub_update(&g_inertial_sample_pub);
}

static void mirror_tx_if_updated(struct bridge_tx_map *map)
{
	uint32_t generation;

	if (map->zros == NULL || map->csyn == NULL) {
		return;
	}
	generation = csyn_zros_generation(map->zros);
	if (generation == 0U || generation == map->last_generation) {
		return;
	}

	if (zros_topic_read(map->zros, map->msg) != 0) {
		return;
	}

	(void)csyn_topic_publish(map->csyn, map->msg, map->msg_size);
	map->last_generation = generation;
}

static void bridge_thread(void *arg0, void *arg1, void *arg2)
{
	struct csyn_topic *manual_topic =
		&topic_manual_control != NULL ? csyn_topic_find("manual") : NULL;
	struct csyn_topic *mocap_topic = &topic_mocap != NULL ? csyn_topic_find("mocap") : NULL;
	struct csyn_topic *inertial_topic =
		&topic_inertial_sample != NULL ? csyn_topic_find("imu") : NULL;
	struct csyn_topic *odometry_topic =
		&topic_odometry != NULL ? csyn_topic_find("odom") : NULL;
	uint32_t last_manual_generation = 0U;
	uint32_t last_mocap_generation = 0U;
	uint32_t last_inertial_generation = 0U;
	uint32_t last_odometry_generation = 0U;

	ARG_UNUSED(arg0);
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);

	while (true) {
		publish_manual_control_if_updated(manual_topic, &last_manual_generation);
		publish_mocap_if_updated(mocap_topic, &last_mocap_generation);
		publish_inertial_sample_if_updated(inertial_topic, &last_inertial_generation);
		publish_odometry_if_updated(odometry_topic, &last_odometry_generation);
		for (size_t i = 0U; i < ARRAY_SIZE(g_tx_maps); i++) {
			mirror_tx_if_updated(&g_tx_maps[i]);
		}
		k_sleep(K_MSEC(1));
	}
}

static int bridge_init(void)
{
	int rc;
	bool has_topics = false;

	/* The application owns the topic list; bridge only what it defined. */
	for (size_t i = 0U; i < ARRAY_SIZE(g_tx_maps); i++) {
		g_tx_maps[i].csyn = csyn_topic_find(g_tx_maps[i].csyn_key);
		if (g_tx_maps[i].zros != NULL && g_tx_maps[i].csyn == NULL) {
			LOG_WRN("csyn topic %s not registered; not bridged", g_tx_maps[i].csyn_key);
		} else if (g_tx_maps[i].zros == NULL && g_tx_maps[i].csyn != NULL) {
			LOG_WRN("zros storage for %s not defined; not bridged",
				g_tx_maps[i].csyn_key);
		} else if (g_tx_maps[i].zros != NULL) {
			has_topics = true;
		}
	}

	zros_node_init(&g_bridge_node, "csyn_zros");

	if (&topic_manual_control != NULL && csyn_topic_find("manual") != NULL) {
		rc = zros_pub_init(&g_manual_control_pub, &g_bridge_node, &topic_manual_control,
				   &g_manual_control);
		if (rc != 0) {
			return rc;
		}
		has_topics = true;
	}

	if (&topic_mocap != NULL && csyn_topic_find("mocap") != NULL) {
		rc = zros_pub_init(&g_mocap_pub, &g_bridge_node, &topic_mocap, &g_mocap);
		if (rc != 0) {
			return rc;
		}
		has_topics = true;
	}

	if (&topic_inertial_sample != NULL && csyn_topic_find("imu") != NULL) {
		rc = zros_pub_init(&g_inertial_sample_pub, &g_bridge_node, &topic_inertial_sample,
				   &g_inertial_sample);
		if (rc != 0) {
			return rc;
		}
		has_topics = true;
	}

	if (&topic_odometry != NULL && csyn_topic_find("odom") != NULL) {
		rc = zros_pub_init(&g_odometry_pub, &g_bridge_node, &topic_odometry, &g_odometry);
		if (rc != 0) {
			return rc;
		}
		has_topics = true;
	}

	if (!has_topics) {
		return 0;
	}

	k_thread_create(&g_bridge_thread, g_bridge_stack, K_THREAD_STACK_SIZEOF(g_bridge_stack),
			bridge_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0,
			K_NO_WAIT);
	k_thread_name_set(&g_bridge_thread, "csyn_zros");

	return 0;
}

SYS_INIT(bridge_init, APPLICATION, 0);
