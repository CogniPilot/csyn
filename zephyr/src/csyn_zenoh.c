/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <csyn/csyn.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zenoh-pico.h>

LOG_MODULE_REGISTER(csyn_zenoh, LOG_LEVEL_INF);

#define CSYN_ZENOH_MAX_TOPICS             32U
#define CSYN_ZENOH_MAX_QUERYABLES         8U
#define CSYN_ZENOH_KEYEXPR_MAX            96U
#define CSYN_VALUE_CONTRACT_MAX           192U
#define CSYN_CONTRACT_WARNING_INTERVAL_MS 10000

static K_THREAD_STACK_DEFINE(g_csyn_zenoh_stack, CONFIG_CSYN_ZENOH_THREAD_STACK_SIZE);
static struct k_thread g_csyn_zenoh_thread;

struct csyn_queryable {
	const char *key;
	/* Catalog command whose request/reply contract the service speaks;
	 * NULL for services outside the command catalog. */
	const synapse_command_info_t *command;
	csyn_query_handler_t handler;
	void *user;
	int64_t last_contract_warning_ms;
};

static struct csyn_queryable g_queryables[CSYN_ZENOH_MAX_QUERYABLES];
static size_t g_queryable_count;

bool csyn_zenoh_register_queryable(const char *key, csyn_query_handler_t handler, void *user)
{
	const char *name;

	if (key == NULL || handler == NULL || g_queryable_count >= ARRAY_SIZE(g_queryables)) {
		return false;
	}

	name = strrchr(key, '/');
	name = (name != NULL) ? name + 1 : key;
	g_queryables[g_queryable_count++] = (struct csyn_queryable){
		.key = key,
		.command = synapse_command_by_name(name),
		.handler = handler,
		.user = user,
	};
	return true;
}

/* Every catalog topic this node carries publishes and subscribes as
 * [<namespace>/]<key> per the synapse key grammar; CONFIG_CSYN_NAMESPACE
 * scopes the whole node, e.g. "cub1".
 */
static int key_with_namespace(const char *key, char *out, size_t out_size)
{
	int len;

	if (CONFIG_CSYN_NAMESPACE[0] != '\0') {
		len = snprintf(out, out_size, "%s/%s", CONFIG_CSYN_NAMESPACE, key);
	} else {
		len = snprintf(out, out_size, "%s", key);
	}

	if (len <= 0 || (size_t)len >= out_size) {
		return -EINVAL;
	}
	return 0;
}

static int topic_keyexpr(const synapse_topic_info_t *info, char *out, size_t out_size)
{
	return key_with_namespace(info->key, out, out_size);
}

static int value_contract(const synapse_topic_info_t *info, char *out, size_t out_size)
{
	const char *media_type =
		info->fixed_layout ? "application/x-synapse-struct" : "application/x-flatbuffers";

	if (info->wire_type == NULL || info->schema_hash == NULL) {
		return -EINVAL;
	}
	return snprintf(out, out_size, "%s;type=%s;schema=sha256-128:%s", media_type,
			info->wire_type, info->schema_hash);
}

/* Request (reply=false) or reply (reply=true) side of a command's value
 * contract, mirroring value_contract() for topics.
 */
static int command_contract(const synapse_command_info_t *command, bool reply, char *out,
			    size_t out_size)
{
	const char *encoding = reply ? command->reply_encoding : command->request_encoding;
	const char *type = reply ? command->reply_type : command->request_type;
	const char *hash = reply ? command->reply_schema_hash : command->request_schema_hash;
	const char *media_type = (encoding != NULL && strcmp(encoding, "struct") == 0)
					 ? "application/x-synapse-struct"
					 : "application/x-flatbuffers";

	if (type == NULL || hash == NULL) {
		return -EINVAL;
	}
	return snprintf(out, out_size, "%s;type=%s;schema=sha256-128:%s", media_type, type, hash);
}

static bool sample_contract_matches(z_loaned_sample_t *sample, struct csyn_topic *topic)
{
	char expected[CSYN_VALUE_CONTRACT_MAX];
	z_owned_string_t received;
	int expected_len = value_contract(topic->info, expected, sizeof(expected));
	bool matches = false;

	z_internal_null(&received);
	if (expected_len > 0 && (size_t)expected_len < sizeof(expected) &&
	    z_encoding_to_string(z_sample_encoding(sample), &received) >= 0) {
		matches = z_string_len(z_loan(received)) == (size_t)expected_len &&
			  memcmp(z_string_data(z_loan(received)), expected, expected_len) == 0;
	}

	if (!matches) {
		int64_t now = k_uptime_get();
		if (topic->last_contract_warning_ms == 0 ||
		    now - topic->last_contract_warning_ms >= CSYN_CONTRACT_WARNING_INTERVAL_MS) {
			LOG_WRN("rejecting %s: incompatible or missing value contract",
				topic->info->key);
			topic->last_contract_warning_ms = MAX(now, 1);
		}
	}
	z_drop(z_move(received));
	return matches;
}

static void input_handler(z_loaned_sample_t *sample, void *arg)
{
	struct csyn_topic *topic = arg;
	z_bytes_reader_t reader;
	uint8_t buf[CONFIG_CSYN_FLATBUFFER_MAX_SIZE];
	size_t payload_len = z_bytes_len(z_sample_payload(sample));

	if (!sample_contract_matches(sample, topic)) {
		return;
	}

	if (payload_len > MIN(sizeof(buf), (size_t)topic->max_size)) {
		LOG_WRN("zenoh payload too large for %s: %zu", topic->key, payload_len);
		return;
	}

	reader = z_bytes_get_reader(z_sample_payload(sample));
	if (z_bytes_reader_read(&reader, buf, payload_len) != payload_len) {
		LOG_WRN("zenoh payload read failed for %s", topic->key);
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
	char key[CSYN_ZENOH_KEYEXPR_MAX];
	int rc = topic_keyexpr(topic->info, key, sizeof(key));

	if (rc < 0) {
		return rc;
	}
	return declare_rx_key_subscriber(session, key, topic);
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

/* Reject catalog-command requests whose value contract is missing or wrong,
 * mirroring sample_contract_matches(). Services outside the command catalog
 * carry no contract.
 */
static bool query_contract_matches(z_loaned_query_t *query, struct csyn_queryable *service)
{
	char expected[CSYN_VALUE_CONTRACT_MAX];
	z_owned_string_t received;
	int expected_len;
	bool matches = false;

	if (service->command == NULL) {
		return true;
	}

	expected_len = command_contract(service->command, false, expected, sizeof(expected));
	z_internal_null(&received);
	if (expected_len > 0 && (size_t)expected_len < sizeof(expected) &&
	    z_encoding_to_string(z_query_encoding(query), &received) >= 0) {
		matches = z_string_len(z_loan(received)) == (size_t)expected_len &&
			  memcmp(z_string_data(z_loan(received)), expected, expected_len) == 0;
	}

	if (!matches) {
		int64_t now = k_uptime_get();
		if (service->last_contract_warning_ms == 0 ||
		    now - service->last_contract_warning_ms >= CSYN_CONTRACT_WARNING_INTERVAL_MS) {
			LOG_WRN("rejecting %s: incompatible or missing request contract",
				service->key);
			service->last_contract_warning_ms = MAX(now, 1);
		}
	}
	z_drop(z_move(received));
	return matches;
}

static void query_handler(z_loaned_query_t *query, void *arg)
{
	struct csyn_queryable *service = arg;
	z_bytes_reader_t reader;
	uint8_t request[CONFIG_CSYN_FLATBUFFER_MAX_SIZE];
	uint8_t reply[CONFIG_CSYN_FLATBUFFER_MAX_SIZE];
	size_t request_len = z_bytes_len(z_query_payload(query));
	size_t reply_len = 0U;
	z_owned_bytes_t reply_bytes;
	z_query_reply_options_t options;
	int rc;

	LOG_DBG("csyn zenoh query %s (%zu bytes)", service->key, request_len);

	if (!query_contract_matches(query, service)) {
		return;
	}
	if (request_len > sizeof(request)) {
		return;
	}
	reader = z_bytes_get_reader(z_query_payload(query));
	if (z_bytes_reader_read(&reader, request, request_len) != request_len) {
		LOG_WRN("csyn zenoh query payload read failed for %s", service->key);
		return;
	}
	if (!service->handler(request, request_len, reply, sizeof(reply), &reply_len,
			      service->user) ||
	    reply_len > sizeof(reply)) {
		LOG_WRN("csyn zenoh query handler failed for %s", service->key);
		return;
	}
	LOG_DBG("csyn zenoh reply %s (%zu bytes)", service->key, reply_len);

	z_query_reply_options_default(&options);
	if (service->command != NULL) {
		char contract[CSYN_VALUE_CONTRACT_MAX];
		z_owned_encoding_t encoding;
		int len = command_contract(service->command, true, contract, sizeof(contract));

		z_internal_null(&encoding);
		if (len > 0 && (size_t)len < sizeof(contract) &&
		    z_encoding_from_str(&encoding, contract) == 0) {
			options.encoding = z_move(encoding);
		}
	}

	z_internal_null(&reply_bytes);
	rc = z_bytes_copy_from_buf(&reply_bytes, reply, reply_len);
	if (rc == 0) {
		rc = z_query_reply(query, z_query_keyexpr(query), z_move(reply_bytes), &options);
	}
	if (rc < 0) {
		LOG_WRN("csyn zenoh reply failed for %s: %d", service->key, rc);
	}
}

static int declare_queryable(const z_loaned_session_t *session, struct csyn_queryable *service)
{
	char key[CSYN_ZENOH_KEYEXPR_MAX];
	z_owned_closure_query_t callback;
	z_view_keyexpr_t view;
	int rc;

	z_internal_null(&callback);

	rc = key_with_namespace(service->key, key, sizeof(key));
	if (rc < 0) {
		return rc;
	}
	rc = z_view_keyexpr_from_str(&view, key);
	if (rc < 0) {
		return rc;
	}
	z_closure_query(&callback, query_handler, NULL, service);
	return z_declare_background_queryable(session, z_loan(view), z_move(callback), NULL);
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

	/* zenoh-pico's multithreaded API does not service inbound traffic until
	 * both background tasks are started. Publications can appear to work
	 * without them, while subscriptions and queryables never receive data.
	 */
	rc = zp_start_read_task(z_loan_mut(*session), NULL);
	if (rc < 0) {
		z_drop(z_move(*session));
		return rc;
	}
	rc = zp_start_lease_task(z_loan_mut(*session), NULL);
	if (rc < 0) {
		z_drop(z_move(*session));
		return rc;
	}

	for (size_t i = 0U; i < csyn_topic_count(); i++) {
		struct csyn_topic *topic = csyn_topic_at(i);

		if (topic->dir != CSYN_DIR_RX || topic->info == NULL) {
			continue;
		}

		/* The frame stream is high-rate; only pull it off the network
		 * when it is the selected mocap source. Mocap streams predate
		 * the value-contract scheme (legacy compact poses carry no
		 * metadata), so they use the arbitration handler instead of
		 * the contract-enforcing one.
		 */
		if (strcmp(topic->key, "mocap") == 0) {
			char key[CSYN_ZENOH_KEYEXPR_MAX];

			if (!IS_ENABLED(CONFIG_CSYN_MOCAP_SOURCE_FRAME)) {
				continue;
			}
			rc = topic_keyexpr(topic->info, key, sizeof(key));
			if (rc == 0) {
				rc = declare_rx_mocap_subscriber(z_loan(*session), key, topic);
			}
		} else {
			rc = declare_rx_subscriber(z_loan(*session), topic);
		}
		if (rc < 0) {
			z_drop(z_move(*session));
			return rc;
		}
	}

	/* Direct mocap feed: subscribe a raw pose stream into the mocap
	 * topic without any ground-side republisher in the path.
	 */
	if (IS_ENABLED(CONFIG_CSYN_MOCAP_SOURCE_POSE_KEY) &&
	    CONFIG_CSYN_ZENOH_MOCAP_POSE_KEY[0] != '\0') {
		struct csyn_topic *mocap = csyn_topic_find("mocap");

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
		struct csyn_topic *odometry = csyn_topic_find("external_pose");

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

	for (size_t i = 0U; i < g_queryable_count; i++) {
		rc = declare_queryable(z_loan(*session), &g_queryables[i]);
		if (rc < 0) {
			z_drop(z_move(*session));
			return rc;
		}
		LOG_INF("csyn zenoh service %s", g_queryables[i].key);
	}

	return 0;
}

static int zenoh_put(const z_loaned_session_t *session, const synapse_topic_info_t *info,
		     const uint8_t *payload, size_t payload_len)
{
	char key[CSYN_ZENOH_KEYEXPR_MAX];
	char contract[CSYN_VALUE_CONTRACT_MAX];
	z_owned_bytes_t bytes;
	z_owned_encoding_t encoding;
	z_view_keyexpr_t view;
	z_put_options_t options;
	int rc;

	z_internal_null(&bytes);
	z_internal_null(&encoding);

	rc = topic_keyexpr(info, key, sizeof(key));
	if (rc < 0) {
		return rc;
	}

	rc = z_view_keyexpr_from_str(&view, key);
	if (rc < 0) {
		return rc;
	}

	rc = z_bytes_copy_from_buf(&bytes, payload, payload_len);
	if (rc < 0) {
		return rc;
	}
	rc = value_contract(info, contract, sizeof(contract));
	if (rc <= 0 || (size_t)rc >= sizeof(contract)) {
		z_drop(z_move(bytes));
		return -EINVAL;
	}
	rc = z_encoding_from_str(&encoding, contract);
	if (rc < 0) {
		z_drop(z_move(bytes));
		return rc;
	}
	z_put_options_default(&options);
	options.encoding = z_move(encoding);

	return z_put(session, z_loan(view), z_move(bytes), &options);
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

	if (zenoh_put(session, topic->info, buf, len) == 0) {
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
