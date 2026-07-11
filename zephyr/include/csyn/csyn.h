#ifndef CSYN_H_
#define CSYN_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/iterable_sections.h>

#include <synapse/topic_catalog.h>

enum csyn_dir {
	CSYN_DIR_RX = 0,
	CSYN_DIR_TX,
};

/*
 * One registered synapse topic with a lock-free latest-sample store.
 * `key` is the vehicle-declared deployment key. `info` resolves its final
 * topic segment against the generated catalog at init, so payload type,
 * encoding, schema, and id match the pinned synapse_fbs release.
 */
struct csyn_topic {
	const char *key;
	const synapse_topic_info_t *info;
	enum csyn_dir dir;
	uint8_t *slots;
	uint16_t max_size;
	uint16_t lengths[2];
	atomic_t generation;
	int64_t last_contract_warning_ms;
};

/*
 * Register one synapse topic in the application's topic list. csyn does not
 * define any topics itself: applications declare the topics they carry, and
 * the store, shell, and transports iterate whatever was declared. `_key` is
 * the exact wire key and may include an arbitrary deployment namespace; its
 * final topic segment must resolve through the synapse_fbs catalog. Init fails
 * on unknown keys or fixed-layout size mismatches. `_max_size` bytes are
 * reserved twice for the double-buffered latest-sample store.
 */
#define CSYN_TOPIC_DEFINE(_name, _key, _dir, _max_size)                                            \
	static uint8_t _csyn_slots_##_name[2U * (_max_size)];                                      \
	STRUCT_SECTION_ITERABLE(csyn_topic, _name) = {                                             \
		.key = (_key),                                                                     \
		.dir = (_dir),                                                                     \
		.slots = _csyn_slots_##_name,                                                      \
		.max_size = (_max_size),                                                           \
	}

size_t csyn_topic_count(void);
struct csyn_topic *csyn_topic_at(size_t idx);
struct csyn_topic *csyn_topic_find(const char *name);
struct csyn_topic *csyn_topic_by_catalog_id(uint16_t id);
bool csyn_topic_publish(struct csyn_topic *topic, const void *buf, size_t len);
bool csyn_topic_copy(struct csyn_topic *topic, void *buf, size_t buf_size, size_t *len,
		     uint32_t *generation);
uint32_t csyn_topic_generation(const struct csyn_topic *topic);

/* A small request/reply surface for vehicle services. The callback runs on
 * csyn's Zenoh transport thread, so it must be bounded and must not block on
 * the control loop. `reply` is caller-owned and valid only for this call.
 *
 * Keys naming catalog commands (`cmd/<name>`) get the mandatory synapse
 * value contract: requests with a missing or mismatched encoding are
 * rejected before the handler runs, and replies are stamped with the
 * command's reply contract. CONFIG_CSYN_NAMESPACE applies to service keys
 * the same way it applies to topic keys.
 */
typedef bool (*csyn_query_handler_t)(const uint8_t *request, size_t request_len, uint8_t *reply,
				     size_t reply_capacity, size_t *reply_len, void *user);

/* Register before the Zenoh transport connects (normally from main). Returns
 * false when the key is invalid or the registry is full.
 */
bool csyn_zenoh_register_queryable(const char *key, csyn_query_handler_t handler, void *user);

#endif
