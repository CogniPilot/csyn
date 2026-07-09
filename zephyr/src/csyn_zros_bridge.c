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

ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(manual_control, struct csyn_manual_control);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(external_odometry, synapse_topic_ExternalOdometryData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(pwm_signal_outputs, synapse_topic_PwmSignalOutputsData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(vehicle_health, synapse_topic_VehicleHealthData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(attitude_estimate, synapse_topic_AttitudeEstimateData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(attitude_command, synapse_topic_AttitudeCommandData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(control_loop_metrics, synapse_topic_ControlLoopMetricsData_t);

uint32_t csyn_zros_generation(const struct zros_topic *topic)
{
	return (uint32_t)atomic_get((atomic_t *)&topic->_lockless_generation);
}

static K_THREAD_STACK_DEFINE(g_bridge_stack, 2048);
static struct k_thread g_bridge_thread;
static struct zros_node g_bridge_node;
static struct zros_pub g_manual_control_pub;
static struct zros_pub g_external_odometry_pub;
static struct csyn_manual_control g_manual_control;
static synapse_topic_ExternalOdometryData_t g_external_odometry;

/*
 * Fixed-layout TX topics carry the same struct bytes on both buses, so
 * mirroring zros -> csyn is an identity copy per topic.
 */
struct bridge_tx_map {
	struct zros_topic *zros;
	const char *csyn_suffix;
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

static struct bridge_tx_map g_tx_maps[] = {
	{&topic_pwm_signal_outputs, "pwm_signal_outputs", &g_pwm_msg, sizeof(g_pwm_msg)},
	{&topic_vehicle_health, "vehicle_health", &g_health_msg, sizeof(g_health_msg)},
	{&topic_attitude_estimate, "attitude_estimate", &g_att_est_msg, sizeof(g_att_est_msg)},
	{&topic_attitude_command, "attitude_command", &g_att_cmd_msg, sizeof(g_att_cmd_msg)},
	{&topic_control_loop_metrics, "control_loop_metrics", &g_metrics_msg,
	 sizeof(g_metrics_msg)},
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

static void publish_external_odometry_if_updated(struct csyn_topic *topic,
						 uint32_t *last_generation)
{
	size_t len = 0U;
	static bool logged_size_fail;
	static bool logged_ok;

	if (topic == NULL || !copy_csyn_topic(topic, (uint8_t *)&g_external_odometry,
					      sizeof(g_external_odometry), &len, last_generation)) {
		return;
	}

	if (len != sizeof(g_external_odometry)) {
		if (!logged_size_fail) {
			LOG_WRN("external_odometry size mismatch len=%u expected=%u",
				(unsigned int)len, (unsigned int)sizeof(g_external_odometry));
			logged_size_fail = true;
		}
		return;
	}

	if (!logged_ok) {
		LOG_INF("external_odometry received pos=[%.3f %.3f %.3f]",
			(double)g_external_odometry.position_enu_m.x,
			(double)g_external_odometry.position_enu_m.y,
			(double)g_external_odometry.position_enu_m.z);
		logged_ok = true;
	}
	(void)zros_pub_update(&g_external_odometry_pub);
}

static void mirror_tx_if_updated(struct bridge_tx_map *map)
{
	uint32_t generation = csyn_zros_generation(map->zros);

	if (map->csyn == NULL || generation == 0U || generation == map->last_generation) {
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
	struct csyn_topic *manual_topic = csyn_topic_find("manual_control_command");
	struct csyn_topic *external_odometry_topic = csyn_topic_find("external_odometry");
	uint32_t last_manual_generation = 0U;
	uint32_t last_external_odometry_generation = 0U;

	ARG_UNUSED(arg0);
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);

	while (true) {
		publish_manual_control_if_updated(manual_topic, &last_manual_generation);
		publish_external_odometry_if_updated(external_odometry_topic,
						     &last_external_odometry_generation);
		for (size_t i = 0U; i < ARRAY_SIZE(g_tx_maps); i++) {
			mirror_tx_if_updated(&g_tx_maps[i]);
		}
		k_sleep(K_MSEC(1));
	}
}

static int bridge_init(void)
{
	int rc;

	for (size_t i = 0U; i < ARRAY_SIZE(g_tx_maps); i++) {
		g_tx_maps[i].csyn = csyn_topic_find(g_tx_maps[i].csyn_suffix);
		if (g_tx_maps[i].csyn == NULL) {
			LOG_ERR("csyn topic %s not registered", g_tx_maps[i].csyn_suffix);
			return -EINVAL;
		}
	}

	zros_node_init(&g_bridge_node, "csyn_zros");

	rc = zros_pub_init(&g_manual_control_pub, &g_bridge_node, &topic_manual_control,
			   &g_manual_control);
	if (rc != 0) {
		return rc;
	}

	rc = zros_pub_init(&g_external_odometry_pub, &g_bridge_node, &topic_external_odometry,
			   &g_external_odometry);
	if (rc != 0) {
		return rc;
	}

	k_thread_create(&g_bridge_thread, g_bridge_stack, K_THREAD_STACK_SIZEOF(g_bridge_stack),
			bridge_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0,
			K_NO_WAIT);
	k_thread_name_set(&g_bridge_thread, "csyn_zros");

	return 0;
}

SYS_INIT(bridge_init, APPLICATION, 0);
