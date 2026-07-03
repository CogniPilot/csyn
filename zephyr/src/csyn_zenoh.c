/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <csyn/csyn.h>

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zenoh-pico.h>

LOG_MODULE_REGISTER(csyn_zenoh, LOG_LEVEL_INF);

#define CSYN_ZENOH_MAX_TOPICS 32U

static K_THREAD_STACK_DEFINE(g_csyn_zenoh_stack, CONFIG_CSYN_ZENOH_THREAD_STACK_SIZE);
static struct k_thread g_csyn_zenoh_thread;

static void input_handler(z_loaned_sample_t *sample, void *arg)
{
	struct csyn_topic *topic = arg;
	z_bytes_reader_t reader;
	uint8_t buf[CONFIG_CSYN_FLATBUFFER_MAX_SIZE];
	size_t payload_len = z_bytes_len(z_sample_payload(sample));

	if (payload_len > MIN(sizeof(buf), (size_t)topic->max_size)) {
		LOG_WRN("zenoh payload too large for %s: %zu", topic->key_suffix, payload_len);
		return;
	}

	reader = z_bytes_get_reader(z_sample_payload(sample));
	if (z_bytes_reader_read(&reader, buf, payload_len) != payload_len) {
		LOG_WRN("zenoh payload read failed for %s", topic->key_suffix);
		return;
	}

	(void)csyn_topic_publish(topic, buf, payload_len);
}

static int config_init(z_owned_config_t *config)
{
	bool is_client = strcmp(CONFIG_CSYN_ZENOH_MODE, "client") == 0;
	uint8_t locator_key = is_client ? Z_CONFIG_CONNECT_KEY : Z_CONFIG_LISTEN_KEY;
	int rc;

	rc = z_config_default(config);
	if (rc < 0) {
		return rc;
	}

	rc = zp_config_insert(z_loan_mut(*config), Z_CONFIG_MODE_KEY, CONFIG_CSYN_ZENOH_MODE);
	if (rc < 0) {
		return rc;
	}

	if (CONFIG_CSYN_ZENOH_LOCATOR[0] != '\0') {
		rc = zp_config_insert(z_loan_mut(*config), locator_key, CONFIG_CSYN_ZENOH_LOCATOR);
	}

	return rc;
}

static int declare_rx_subscriber(const z_loaned_session_t *session, struct csyn_topic *topic)
{
	z_owned_closure_sample_t callback;
	z_view_keyexpr_t view;
	int rc;

	z_internal_null(&callback);

	rc = z_view_keyexpr_from_str(&view, topic->info->key);
	if (rc < 0) {
		return rc;
	}

	z_closure(&callback, input_handler, NULL, topic);
	return z_declare_background_subscriber(session, z_loan(view), z_move(callback), NULL);
}

static int open_session(z_owned_session_t *session)
{
	z_owned_config_t config;
	int rc;

	z_internal_null(&config);
	z_internal_null(session);

	rc = config_init(&config);
	if (rc < 0) {
		return rc;
	}

	rc = z_open(session, z_move(config), NULL);
	if (rc < 0) {
		return rc;
	}

	for (size_t i = 0U; i < csyn_topic_count(); i++) {
		struct csyn_topic *topic = csyn_topic_at(i);

		if (topic->dir != CSYN_DIR_RX || topic->info == NULL) {
			continue;
		}

		rc = declare_rx_subscriber(z_loan(*session), topic);
		if (rc < 0) {
			z_drop(z_move(*session));
			return rc;
		}
	}

	return 0;
}

static int zenoh_put(const z_loaned_session_t *session, const char *keyexpr, const uint8_t *payload,
		     size_t payload_len)
{
	z_owned_bytes_t bytes;
	z_view_keyexpr_t view;
	int rc;

	z_internal_null(&bytes);

	rc = z_view_keyexpr_from_str(&view, keyexpr);
	if (rc < 0) {
		return rc;
	}

	rc = z_bytes_copy_from_buf(&bytes, payload, payload_len);
	if (rc < 0) {
		return rc;
	}

	return z_put(session, z_loan(view), z_move(bytes), NULL);
}

static void put_topic_if_updated(const z_loaned_session_t *session, struct csyn_topic *topic,
				 uint32_t *last_generation)
{
	uint8_t buf[CONFIG_CSYN_FLATBUFFER_MAX_SIZE];
	size_t len;
	uint32_t generation = csyn_topic_generation(topic);

	if (topic->info == NULL || generation == 0U || generation == *last_generation) {
		return;
	}

	if (!csyn_topic_copy(topic, buf, sizeof(buf), &len, NULL)) {
		return;
	}

	if (zenoh_put(session, topic->info->key, buf, len) == 0) {
		*last_generation = generation;
	}
}

static void csyn_zenoh_thread(void *arg0, void *arg1, void *arg2)
{
	ARG_UNUSED(arg0);
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);

	BUILD_ASSERT(CSYN_ZENOH_MAX_TOPICS >= 1U);

	while (true) {
		z_owned_session_t session;
		uint32_t last_generation[CSYN_ZENOH_MAX_TOPICS] = {0};
		int rc = open_session(&session);

		if (rc < 0) {
			LOG_WRN("csyn zenoh open failed: %d", rc);
			k_sleep(K_MSEC(CONFIG_CSYN_ZENOH_RETRY_MS));
			continue;
		}

		LOG_INF("csyn zenoh %s %s", CONFIG_CSYN_ZENOH_MODE, CONFIG_CSYN_ZENOH_LOCATOR);

		if (csyn_topic_count() > CSYN_ZENOH_MAX_TOPICS) {
			LOG_ERR("too many csyn topics for zenoh transport");
			z_drop(z_move(session));
			return;
		}

		while (!z_session_is_closed(z_loan(session))) {
			for (size_t i = 0U; i < csyn_topic_count(); i++) {
				struct csyn_topic *topic = csyn_topic_at(i);

				if (topic->dir == CSYN_DIR_TX) {
					put_topic_if_updated(z_loan(session), topic,
							     &last_generation[i]);
				}
			}
			k_sleep(K_MSEC(1));
		}

		z_drop(z_move(session));
		k_sleep(K_MSEC(CONFIG_CSYN_ZENOH_RETRY_MS));
	}
}

static int csyn_zenoh_init(void)
{
	k_thread_create(&g_csyn_zenoh_thread, g_csyn_zenoh_stack,
			K_THREAD_STACK_SIZEOF(g_csyn_zenoh_stack), csyn_zenoh_thread, NULL, NULL,
			NULL, CONFIG_CSYN_ZENOH_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&g_csyn_zenoh_thread, "csyn_zenoh");

	return 0;
}

SYS_INIT(csyn_zenoh_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
