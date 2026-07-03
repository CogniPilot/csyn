#ifndef CSYN_H_
#define CSYN_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/atomic.h>

#include <synapse/topic_catalog.h>

enum csyn_dir {
	CSYN_DIR_RX = 0,
	CSYN_DIR_TX,
};

/*
 * One registered synapse topic with a lock-free latest-sample store.
 * `info` resolves against the generated synapse topic catalog at init,
 * so name, keyexpr, payload type, and encoding always match the pinned
 * synapse_fbs release.
 */
struct csyn_topic {
	const char *key_suffix;
	const synapse_topic_info_t *info;
	enum csyn_dir dir;
	uint8_t *slots;
	uint16_t max_size;
	uint16_t lengths[2];
	atomic_t generation;
};

size_t csyn_topic_count(void);
struct csyn_topic *csyn_topic_at(size_t idx);
struct csyn_topic *csyn_topic_find(const char *name);
struct csyn_topic *csyn_topic_by_catalog_id(uint16_t id);
bool csyn_topic_publish(struct csyn_topic *topic, const void *buf, size_t len);
bool csyn_topic_copy(struct csyn_topic *topic, void *buf, size_t buf_size, size_t *len,
		     uint32_t *generation);
uint32_t csyn_topic_generation(const struct csyn_topic *topic);

#endif
