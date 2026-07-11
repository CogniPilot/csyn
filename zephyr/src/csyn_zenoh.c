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

static bool key_contains(const char *key, size_t key_len, const char *needle)
{
	size_t needle_len = strlen(needle);

	if (key == NULL || key_len < needle_len) {
		return false;
	}
	for (size_t i = 0U; i + needle_len <= key_len; i++) {
		if (memcmp(&key[i], needle, needle_len) == 0) {
			return true;
		}
	}
	return false;
}

/* Mocap arbitration: the selected/ stream is the station's deliberate source
 * choice, so while it delivers plausible poses the fallback keys are ignored.
 */
static void mocap_input_handler(z_loaned_sample_t *sample, void *arg)
{
	struct csyn_topic *topic = arg;
	z_bytes_reader_t reader;
	z_view_string_t key_view;
	uint8_t buf[CONFIG_CSYN_FLATBUFFER_MAX_SIZE];
	size_t payload_len = z_bytes_len(z_sample_payload(sample));
	/* Sample-count freshness: no kernel clock — this callback runs on the
	 * zenoh rx thread, which is not a Zephyr thread on native_sim.
	 */
	static int suppress_fallback;
	const char *key;
	size_t key_len;
	bool selected;
	bool plausible;

	if (payload_len > MIN(sizeof(buf), (size_t)topic->max_size)) {
		return;
	}
	reader = z_bytes_get_reader(z_sample_payload(sample));
	if (z_bytes_reader_read(&reader, buf, payload_len) != payload_len) {
		return;
	}

	/* all-0xff header marks the tracking-lost sentinel */
	plausible = payload_len >= 4U &&
		    !(buf[0] == 0xffU && buf[1] == 0xffU && buf[2] == 0xffU && buf[3] == 0xffU);

	z_keyexpr_as_view_string(z_sample_keyexpr(sample), &key_view);
	key = z_string_data(z_loan(key_view));
	key_len = z_string_len(z_loan(key_view));
	selected = key_contains(key, key_len, "/selected/");

	if (selected) {
		if (plausible) {
			suppress_fallback = CONFIG_CSYN_ZENOH_MOCAP_SELECTED_HOLD_SAMPLES;
		}
	} else if (suppress_fallback > 0) {
		suppress_fallback--;
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

static int declare_rx_key_subscriber(const z_loaned_session_t *session, const char *key,
				     struct csyn_topic *topic)
{
	z_owned_closure_sample_t callback;
	z_view_keyexpr_t view;
	int rc;

	z_internal_null(&callback);

	rc = z_view_keyexpr_from_str(&view, key);
	if (rc < 0) {
		return rc;
	}

	z_closure(&callback, input_handler, NULL, topic);
	return z_declare_background_subscriber(session, z_loan(view), z_move(callback), NULL);
}

static int declare_rx_subscriber(const z_loaned_session_t *session, struct csyn_topic *topic)
{
	return declare_rx_key_subscriber(session, topic->info->key, topic);
}

static int declare_rx_mocap_subscriber(const z_loaned_session_t *session, const char *key,
				       struct csyn_topic *topic)
{
	z_owned_closure_sample_t callback;
	z_view_keyexpr_t view;
	int rc;

	z_internal_null(&callback);

	rc = z_view_keyexpr_from_str(&view, key);
	if (rc < 0) {
		return rc;
	}

	z_closure(&callback, mocap_input_handler, NULL, topic);
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

		/* The frame stream is high-rate; only pull it off the network
		 * when it is the selected mocap source.
		 */
		if (!IS_ENABLED(CONFIG_CSYN_MOCAP_SOURCE_FRAME) &&
		    strcmp(topic->key_suffix, "mocap_frame") == 0) {
			continue;
		}

		rc = declare_rx_subscriber(z_loan(*session), topic);
		if (rc < 0) {
			z_drop(z_move(*session));
			return rc;
		}
	}

	/* Direct mocap feed: subscribe a raw pose stream into the mocap_frame
	 * topic without any ground-side republisher in the path.
	 */
	if (IS_ENABLED(CONFIG_CSYN_MOCAP_SOURCE_POSE_KEY) &&
	    CONFIG_CSYN_ZENOH_MOCAP_POSE_KEY[0] != '\0') {
		struct csyn_topic *mocap = csyn_topic_find("mocap_frame");

		if (mocap != NULL) {
			rc = declare_rx_mocap_subscriber(z_loan(*session),
							 CONFIG_CSYN_ZENOH_MOCAP_POSE_KEY, mocap);
			if (rc < 0) {
				z_drop(z_move(*session));
				return rc;
			}
			LOG_INF("csyn zenoh mocap key %s", CONFIG_CSYN_ZENOH_MOCAP_POSE_KEY);
		}
	}

	/* Per-body estimator stream; subscribing also switches on the bridge's
	 * demand-driven publisher for that body.
	 */
	if (CONFIG_CSYN_ZENOH_EXTERNAL_ODOMETRY_KEY[0] != '\0') {
		struct csyn_topic *odometry = csyn_topic_find("external_odometry");

		if (odometry != NULL) {
			rc = declare_rx_key_subscriber(z_loan(*session),
						       CONFIG_CSYN_ZENOH_EXTERNAL_ODOMETRY_KEY,
						       odometry);
			if (rc < 0) {
				z_drop(z_move(*session));
				return rc;
			}
			LOG_INF("csyn zenoh external odometry key %s",
				CONFIG_CSYN_ZENOH_EXTERNAL_ODOMETRY_KEY);
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
