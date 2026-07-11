/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <csyn/csyn.h>

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <synapse/control_reader.h>
#include <synapse/sensors_reader.h>
#include <synapse/state_reader.h>

LOG_MODULE_REGISTER(csyn, LOG_LEVEL_INF);

#define CSYN_SLOTS(_sym, _size) static uint8_t _sym[2U * (_size)]

CSYN_SLOTS(g_manual_control_slots, sizeof(synapse_topic_ManualControlData_t));
CSYN_SLOTS(g_inertial_sample_slots, sizeof(synapse_topic_InertialSampleData_t));
CSYN_SLOTS(g_external_odometry_slots, sizeof(synapse_topic_ExternalOdometryData_t));
CSYN_SLOTS(g_pwm_signal_outputs_slots, sizeof(synapse_topic_PwmSignalOutputsData_t));
CSYN_SLOTS(g_vehicle_health_slots, sizeof(synapse_topic_VehicleHealthData_t));
CSYN_SLOTS(g_attitude_estimate_slots, sizeof(synapse_topic_AttitudeEstimateData_t));
CSYN_SLOTS(g_attitude_command_slots, sizeof(synapse_topic_AttitudeCommandData_t));
CSYN_SLOTS(g_control_loop_metrics_slots, sizeof(synapse_topic_ControlLoopMetricsData_t));

#define CSYN_TOPIC(_key, _dir, _slots)                                                             \
	{                                                                                          \
		.key = _key,                                                                       \
		.dir = _dir,                                                                       \
		.slots = _slots,                                                                   \
		.max_size = sizeof(_slots) / 2U,                                                   \
	}

static struct csyn_topic g_topics[] = {
	CSYN_TOPIC("manual", CSYN_DIR_RX, g_manual_control_slots),
	CSYN_TOPIC("imu", CSYN_DIR_RX, g_inertial_sample_slots),
	CSYN_TOPIC("external_pose", CSYN_DIR_RX, g_external_odometry_slots),
	CSYN_TOPIC("pwm", CSYN_DIR_TX, g_pwm_signal_outputs_slots),
	CSYN_TOPIC("health", CSYN_DIR_TX, g_vehicle_health_slots),
	CSYN_TOPIC("att", CSYN_DIR_TX, g_attitude_estimate_slots),
	CSYN_TOPIC("att_sp", CSYN_DIR_TX, g_attitude_command_slots),
	CSYN_TOPIC("loop", CSYN_DIR_TX, g_control_loop_metrics_slots),
};

size_t csyn_topic_count(void)
{
	return ARRAY_SIZE(g_topics);
}

struct csyn_topic *csyn_topic_at(size_t idx)
{
	if (idx >= ARRAY_SIZE(g_topics)) {
		return NULL;
	}

	return &g_topics[idx];
}

struct csyn_topic *csyn_topic_find(const char *name)
{
	if (name == NULL) {
		return NULL;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(g_topics); i++) {
		struct csyn_topic *topic = &g_topics[i];

		if (strcmp(name, topic->key) == 0 ||
		    (topic->info != NULL && (strcmp(name, topic->info->name) == 0 ||
					     strcmp(name, topic->info->key) == 0))) {
			return topic;
		}
	}

	return NULL;
}

struct csyn_topic *csyn_topic_by_catalog_id(uint16_t id)
{
	for (size_t i = 0U; i < ARRAY_SIZE(g_topics); i++) {
		if (g_topics[i].info != NULL && g_topics[i].info->id == id) {
			return &g_topics[i];
		}
	}

	return NULL;
}

bool csyn_topic_publish(struct csyn_topic *topic, const void *buf, size_t len)
{
	uint32_t next_generation;
	uint32_t slot;

	if (topic == NULL || buf == NULL || len == 0U || len > topic->max_size) {
		return false;
	}

	next_generation = (uint32_t)atomic_get(&topic->generation) + 1U;
	slot = next_generation & 1U;

	memcpy(&topic->slots[slot * topic->max_size], buf, len);
	topic->lengths[slot] = (uint16_t)len;
	atomic_set(&topic->generation, (atomic_val_t)next_generation);

	return true;
}

bool csyn_topic_copy(struct csyn_topic *topic, void *buf, size_t buf_size, size_t *len,
		     uint32_t *generation)
{
	uint32_t generation_start;
	uint32_t generation_end;
	uint32_t slot;
	uint16_t length;

	if (topic == NULL || buf == NULL || len == NULL) {
		return false;
	}

	do {
		generation_start = (uint32_t)atomic_get(&topic->generation);
		if (generation_start == 0U) {
			return false;
		}

		slot = generation_start & 1U;
		length = topic->lengths[slot];
		if (length == 0U || length > buf_size) {
			return false;
		}

		memcpy(buf, &topic->slots[slot * topic->max_size], length);
		generation_end = (uint32_t)atomic_get(&topic->generation);
	} while (generation_start != generation_end);

	*len = length;
	if (generation != NULL) {
		*generation = generation_start;
	}

	return true;
}

uint32_t csyn_topic_generation(const struct csyn_topic *topic)
{
	if (topic == NULL) {
		return 0U;
	}

	return (uint32_t)atomic_get((atomic_t *)&topic->generation);
}

static int csyn_init(void)
{
	for (size_t i = 0U; i < ARRAY_SIZE(g_topics); i++) {
		struct csyn_topic *topic = &g_topics[i];

		for (size_t j = 0U; j < synapse_topics_count; j++) {
			if (strcmp(synapse_topics[j].key, topic->key) == 0) {
				topic->info = &synapse_topics[j];
				break;
			}
		}

		if (topic->info == NULL) {
			LOG_ERR("topic %s missing from synapse catalog", topic->key);
			return -EINVAL;
		}

		if (topic->info->fixed_layout && topic->info->payload_size != topic->max_size) {
			LOG_ERR("topic %s size mismatch: catalog %u local %u", topic->key,
				(unsigned int)topic->info->payload_size,
				(unsigned int)topic->max_size);
			return -EINVAL;
		}
	}

	return 0;
}

SYS_INIT(csyn_init, POST_KERNEL, 0);
