/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <csyn/csyn.h>
#include <csyn/csyn_types.h>

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(csyn, LOG_LEVEL_INF);

size_t csyn_topic_count(void)
{
	size_t count;

	STRUCT_SECTION_COUNT(csyn_topic, &count);
	return count;
}

struct csyn_topic *csyn_topic_at(size_t idx)
{
	struct csyn_topic *topic;

	if (idx >= csyn_topic_count()) {
		return NULL;
	}

	STRUCT_SECTION_GET(csyn_topic, idx, &topic);
	return topic;
}

struct csyn_topic *csyn_topic_find(const char *name)
{
	if (name == NULL) {
		return NULL;
	}

	STRUCT_SECTION_FOREACH(csyn_topic, topic) {
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
	STRUCT_SECTION_FOREACH(csyn_topic, topic) {
		if (topic->info != NULL && topic->info->id == id) {
			return topic;
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

/* synapse_fbs 0.3.3 catalog entry: VehicleCommand left the 0.5.x schema for
 * the queryable command protocol, but the electrode ground station still
 * consumes this 40-byte broadcast on the old topic key. The id sits above
 * the generated catalog range (0.3.3 used 22, which later releases
 * reassigned), and the schema fingerprint is a fixed marker for this legacy
 * layout — the ground station never validates the value contract.
 */
static const synapse_topic_info_t g_vehicle_command_info = {
	.id = 65001,
	.name = "VehicleCommand",
	.key = "synapse/v1/topic/vehicle_command",
	.root_table = "VehicleCommand",
	.payload_type = "VehicleCommandData",
	.payload_size = sizeof(struct csyn_vehicle_command),
	.schema_file = "fbs/control.fbs",
	.wire_type = "synapse.topic.VehicleCommandData",
	.schema_hash = "ffc2edbf80cfc1e45fd00c6d9443bc0f",
	.fixed_layout = true,
	.multi_instance = false,
	.scope = "any",
	.encoding = "struct",
	.description = "Generic command with floating-point arguments.",
};

static int csyn_init(void)
{
	STRUCT_SECTION_FOREACH(csyn_topic, topic) {
		const char *key_segment;

		topic->info = synapse_topic_by_key(topic->key);
		key_segment = strrchr(topic->key, '/');
		key_segment = key_segment != NULL ? key_segment + 1 : topic->key;

		if (topic->info == NULL && strcmp(key_segment, "vehicle_command") == 0) {
			topic->info = &g_vehicle_command_info;
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
