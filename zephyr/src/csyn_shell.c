/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <csyn/csyn.h>
#include <csyn/csyn_codec.h>
#include <synapse/topic_print.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#define CSYN_HEX_BYTES_PER_LINE 16U

enum csyn_watch_mode {
	CSYN_WATCH_NONE = 0,
	CSYN_WATCH_ECHO,
	CSYN_WATCH_HZ,
	CSYN_WATCH_LINE,
};

struct csyn_watch {
	const struct shell *sh;
	struct csyn_topic *topic;
	enum csyn_watch_mode mode;
	uint32_t period_ms;
	uint32_t last_generation;
	int64_t last_ms;
	bool active;
};

static struct csyn_watch g_csyn_watch;
static uint8_t g_csyn_shell_buf[CONFIG_CSYN_FLATBUFFER_MAX_SIZE];
static char g_csyn_shell_line[512];
K_SEM_DEFINE(g_csyn_watch_sem, 0, 1);

static void csyn_watch_thread(void *p0, void *p1, void *p2);
static void csyn_topic_dynamic_get(size_t idx, struct shell_static_entry *entry);

K_THREAD_DEFINE(g_csyn_watch_tid, CONFIG_CSYN_SHELL_THREAD_STACK_SIZE, csyn_watch_thread, NULL,
		NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
SHELL_DYNAMIC_CMD_CREATE(sub_csyn_topic_names, csyn_topic_dynamic_get);

static void csyn_topic_dynamic_get(size_t idx, struct shell_static_entry *entry)
{
	struct csyn_topic *topic = csyn_topic_at(idx);

	if (topic == NULL) {
		entry->syntax = NULL;
		return;
	}

	entry->syntax = topic->key;
	entry->handler = NULL;
	entry->subcmd = NULL;
	entry->help = NULL;
}

static int parse_u32_arg(const char *arg, uint32_t *out)
{
	char *end = NULL;
	long value = strtol(arg, &end, 10);

	if (*arg == '\0' || *end != '\0' || value <= 0 || value > INT32_MAX) {
		return -EINVAL;
	}

	*out = (uint32_t)value;
	return 0;
}

static void print_hex(const struct shell *sh, const uint8_t *buf, size_t len)
{
	for (size_t offset = 0U; offset < len; offset += CSYN_HEX_BYTES_PER_LINE) {
		char line[(CSYN_HEX_BYTES_PER_LINE * 3U) + 1U];
		size_t line_len = 0U;
		size_t chunk_len = MIN(CSYN_HEX_BYTES_PER_LINE, len - offset);

		for (size_t i = 0U; i < chunk_len; i++) {
			line_len +=
				(size_t)snprintk(&line[line_len], sizeof(line) - line_len, "%02x%s",
						 buf[offset + i], (i + 1U < chunk_len) ? " " : "");
		}

		shell_print(sh, "+0x%04x: %s", (unsigned int)offset, line);
	}
}

static void print_mocap(const struct shell *sh, const uint8_t *buf, size_t len)
{
	csyn_mocap_rigid_body_t rb;

	if (!csyn_decode_mocap_frame(buf, len, &rb)) {
		shell_error(sh, "mocap: failed to decode MocapFrame rigid_bodies[0]");
		print_hex(sh, buf, len);
		return;
	}

	shell_print(sh, "mocap valid=%d pos=[%.3f %.3f %.3f]", rb.valid ? 1 : 0, (double)rb.x,
		    (double)rb.y, (double)rb.z);
	shell_print(sh, "mocap quat=[%.3f %.3f %.3f %.3f] id=%ld", (double)rb.qw, (double)rb.qx,
		    (double)rb.qy, (double)rb.qz, (long)rb.id);
}

static void csyn_topic_print(const struct shell *sh, const struct csyn_topic *topic,
			     const uint8_t *buf, size_t len)
{
	if (topic->info != NULL &&
	    synapse_topic_snprint(g_csyn_shell_line, sizeof(g_csyn_shell_line), topic->info->id,
				  buf, len) >= 0) {
		shell_print(sh, "%s", g_csyn_shell_line);
		return;
	}

	if (strcmp(topic->key, "mocap") == 0) {
		print_mocap(sh, buf, len);
		return;
	}

	print_hex(sh, buf, len);
}

static void csyn_topic_line_once(const struct shell *sh, struct csyn_topic *topic)
{
	char line[256];
	char rendered[256];
	size_t len = 0U;
	uint32_t generation = 0U;
	const char *suffix = topic->key;

	if (!csyn_topic_copy(topic, g_csyn_shell_buf, sizeof(g_csyn_shell_buf), &len,
			     &generation)) {
		(void)snprintk(line, sizeof(line), "%s: no samples", suffix);
		shell_fprintf(sh, SHELL_NORMAL, "\r%-255s", line);
		return;
	}

	if (topic->info != NULL &&
	    synapse_topic_snprint(rendered, sizeof(rendered), topic->info->id, g_csyn_shell_buf,
				  len) >= 0) {
		(void)snprintk(line, sizeof(line), "%s gen=%u %s", suffix, (unsigned int)generation,
			       rendered);
	} else if (strcmp(suffix, "mocap") == 0) {
		csyn_mocap_rigid_body_t rb;

		if (csyn_decode_mocap_frame(g_csyn_shell_buf, len, &rb)) {
			(void)snprintk(line, sizeof(line),
				       "%s gen=%u valid=%d pos=[%.2f %.2f %.2f] quat=[%.2f %.2f "
				       "%.2f %.2f]",
				       suffix, (unsigned int)generation, rb.valid ? 1 : 0,
				       (double)rb.x, (double)rb.y, (double)rb.z, (double)rb.qw,
				       (double)rb.qx, (double)rb.qy, (double)rb.qz);
		} else {
			(void)snprintk(line, sizeof(line), "%s gen=%u len=%u decode=failed", suffix,
				       (unsigned int)generation, (unsigned int)len);
		}
	} else {
		(void)snprintk(line, sizeof(line), "%s gen=%u len=%u name=%s", suffix,
			       (unsigned int)generation, (unsigned int)len,
			       topic->info != NULL ? topic->info->name : "?");
	}

	shell_fprintf(sh, SHELL_NORMAL, "\r%-255s", line);
}

static int csyn_topic_echo_once(const struct shell *sh, struct csyn_topic *topic)
{
	size_t len;
	uint32_t generation;

	if (!csyn_topic_copy(topic, g_csyn_shell_buf, sizeof(g_csyn_shell_buf), &len,
			     &generation)) {
		shell_print(sh, "%s: no samples", topic->key);
		return 0;
	}

	shell_print(sh, "%s gen=%u len=%u type=%s name=%s", topic->key, (unsigned int)generation,
		    (unsigned int)len, topic->info != NULL ? topic->info->payload_type : "?",
		    topic->info != NULL ? topic->info->name : "?");
	csyn_topic_print(sh, topic, g_csyn_shell_buf, len);

	return 0;
}

static void csyn_watch_stop(void)
{
	unsigned int key = irq_lock();

	g_csyn_watch.active = false;
	g_csyn_watch.mode = CSYN_WATCH_NONE;
	g_csyn_watch.topic = NULL;
	g_csyn_watch.sh = NULL;
	g_csyn_watch.period_ms = 0U;
	g_csyn_watch.last_generation = 0U;
	g_csyn_watch.last_ms = 0;

	irq_unlock(key);

	k_sem_give(&g_csyn_watch_sem);
}

static void csyn_watch_start(const struct shell *sh, struct csyn_topic *topic,
			     enum csyn_watch_mode mode, uint32_t period_ms)
{
	unsigned int key = irq_lock();

	g_csyn_watch.sh = sh;
	g_csyn_watch.topic = topic;
	g_csyn_watch.mode = mode;
	g_csyn_watch.period_ms = period_ms;
	g_csyn_watch.last_generation = csyn_topic_generation(topic);
	g_csyn_watch.last_ms = k_uptime_get();
	g_csyn_watch.active = true;

	irq_unlock(key);

	k_sem_give(&g_csyn_watch_sem);
}

static void csyn_watch_thread(void *p0, void *p1, void *p2)
{
	struct csyn_watch watch;

	ARG_UNUSED(p0);
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);

	while (true) {
		(void)k_sem_take(&g_csyn_watch_sem, K_FOREVER);

		while (true) {
			unsigned int key = irq_lock();

			watch = g_csyn_watch;
			irq_unlock(key);

			if (!watch.active || watch.sh == NULL || watch.topic == NULL) {
				break;
			}

			if (watch.mode == CSYN_WATCH_ECHO) {
				(void)csyn_topic_echo_once(watch.sh, watch.topic);
				if (k_sem_take(&g_csyn_watch_sem, K_MSEC(watch.period_ms)) == 0) {
					continue;
				}
			} else if (watch.mode == CSYN_WATCH_HZ) {
				uint32_t generation_now;
				int64_t now_ms;

				if (k_sem_take(&g_csyn_watch_sem, K_MSEC(watch.period_ms)) == 0) {
					continue;
				}

				key = irq_lock();
				watch = g_csyn_watch;
				irq_unlock(key);

				if (!watch.active || watch.sh == NULL || watch.topic == NULL) {
					break;
				}

				generation_now = csyn_topic_generation(watch.topic);
				now_ms = k_uptime_get();
				if (now_ms <= watch.last_ms) {
					now_ms = watch.last_ms + 1;
				}

				shell_print(watch.sh, "%s: %u samples in %lld ms = %0.2f Hz",
					    watch.topic->key,
					    (unsigned int)(generation_now - watch.last_generation),
					    (long long)(now_ms - watch.last_ms),
					    ((double)(generation_now - watch.last_generation) *
					     1000.0) /
						    (double)(now_ms - watch.last_ms));

				key = irq_lock();
				if (g_csyn_watch.active && g_csyn_watch.mode == CSYN_WATCH_HZ &&
				    g_csyn_watch.topic == watch.topic) {
					g_csyn_watch.last_generation = generation_now;
					g_csyn_watch.last_ms = now_ms;
				}
				irq_unlock(key);
			} else if (watch.mode == CSYN_WATCH_LINE) {
				csyn_topic_line_once(watch.sh, watch.topic);
				if (k_sem_take(&g_csyn_watch_sem, K_MSEC(watch.period_ms)) == 0) {
					continue;
				}
			} else {
				break;
			}
		}
	}
}

static int cmd_csyn_topic_list(const struct shell *sh, size_t argc, char **argv)
{
	bool show_live_only = false;
	const char *filter = NULL;
	size_t shown = 0U;

	for (size_t i = 1U; i < argc; i++) {
		if (strcmp(argv[i], "live") == 0) {
			show_live_only = true;
		} else {
			filter = argv[i];
		}
	}

	for (size_t i = 0U; i < csyn_topic_count(); i++) {
		struct csyn_topic *topic = csyn_topic_at(i);
		size_t len = 0U;
		uint32_t generation = 0U;
		bool available = csyn_topic_copy(topic, g_csyn_shell_buf, sizeof(g_csyn_shell_buf),
						 &len, &generation);

		if (show_live_only && !available) {
			continue;
		}

		if (filter != NULL && strstr(topic->key, filter) == NULL &&
		    (topic->info == NULL || strstr(topic->info->name, filter) == NULL)) {
			continue;
		}

		shell_print(sh, "%-24s %-10s %-2s %-5s gen=%u len=%u max=%u name=%s", topic->key,
			    topic->info != NULL ? topic->info->encoding : "?",
			    topic->dir == CSYN_DIR_RX ? "rx" : "tx", available ? "live" : "empty",
			    (unsigned int)generation, (unsigned int)len,
			    (unsigned int)topic->max_size,
			    topic->info != NULL ? topic->info->name : "?");
		shown++;
	}

	if (shown == 0U) {
		shell_print(sh, "no csyn topics match");
	}

	return 0;
}

static int cmd_csyn_topic_info(const struct shell *sh, size_t argc, char **argv)
{
	struct csyn_topic *topic = csyn_topic_find(argv[1]);

	ARG_UNUSED(argc);

	if (topic == NULL) {
		shell_error(sh, "unknown topic: %s", argv[1]);
		return -ENOENT;
	}

	shell_print(sh, "key=%s", topic->key);
	shell_print(sh, "dir=%s", topic->dir == CSYN_DIR_RX ? "rx" : "tx");
	shell_print(sh, "max_size=%u", (unsigned int)topic->max_size);
	shell_print(sh, "generation=%u", (unsigned int)csyn_topic_generation(topic));
	if (topic->info != NULL) {
		shell_print(sh, "name=%s", topic->info->name);
		shell_print(sh, "catalog_id=%u", (unsigned int)topic->info->id);
		shell_print(sh, "type=%s", topic->info->payload_type);
		shell_print(sh, "encoding=%s", topic->info->encoding);
		shell_print(sh, "schema=%s", topic->info->schema_file);
		shell_print(sh, "description=%s", topic->info->description);
	}

	return 0;
}

static int cmd_csyn_topic_echo(const struct shell *sh, size_t argc, char **argv)
{
	struct csyn_topic *topic = csyn_topic_find(argv[1]);
	uint32_t period_ms = 0U;
	int rc;

	if (topic == NULL) {
		shell_error(sh, "unknown topic: %s", argv[1]);
		return -ENOENT;
	}

	if (argc >= 3) {
		rc = parse_u32_arg(argv[2], &period_ms);
		if (rc != 0) {
			shell_error(sh, "period_ms must be a positive integer");
			return rc;
		}
	}

	if (period_ms == 0U) {
		return csyn_topic_echo_once(sh, topic);
	}

	csyn_watch_start(sh, topic, CSYN_WATCH_ECHO, period_ms);
	shell_print(sh, "echoing %s every %u ms; use 'csyn topic stop' to stop", topic->key,
		    (unsigned int)period_ms);

	return 0;
}

static int cmd_csyn_topic_hz(const struct shell *sh, size_t argc, char **argv)
{
	struct csyn_topic *topic = csyn_topic_find(argv[1]);
	uint32_t period_ms = 1000U;
	int rc;

	if (topic == NULL) {
		shell_error(sh, "unknown topic: %s", argv[1]);
		return -ENOENT;
	}

	if (argc >= 3) {
		rc = parse_u32_arg(argv[2], &period_ms);
		if (rc != 0) {
			shell_error(sh, "window_ms must be a positive integer");
			return rc;
		}
	}

	csyn_watch_start(sh, topic, CSYN_WATCH_HZ, period_ms);
	shell_print(sh, "measuring %s every %u ms; use 'csyn topic stop' to stop", topic->key,
		    (unsigned int)period_ms);

	return 0;
}

static int cmd_csyn_topic_watch(const struct shell *sh, size_t argc, char **argv)
{
	struct csyn_topic *topic = csyn_topic_find(argv[1]);
	uint32_t period_ms = 250U;
	int rc;

	if (topic == NULL) {
		shell_error(sh, "unknown topic: %s", argv[1]);
		return -ENOENT;
	}

	if (argc >= 3) {
		rc = parse_u32_arg(argv[2], &period_ms);
		if (rc != 0) {
			shell_error(sh, "period_ms must be a positive integer");
			return rc;
		}
	}

	csyn_watch_start(sh, topic, CSYN_WATCH_LINE, period_ms);
	shell_print(sh, "watching %s every %u ms; use 'csyn topic stop' to stop", topic->key,
		    (unsigned int)period_ms);

	return 0;
}

static int cmd_csyn_topic_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	csyn_watch_stop();
	shell_fprintf(sh, SHELL_NORMAL, "\n");
	shell_print(sh, "csyn topic watcher stopped");

	return 0;
}

static int cmd_csyn_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "csyn: enabled");
	shell_print(sh, "catalog: %u synapse topics, %u registered",
		    (unsigned int)synapse_topics_count, (unsigned int)csyn_topic_count());
#if defined(CONFIG_CSYN_NATIVE_UDP)
	shell_print(sh, "transport: native_sim UDP rx=%d tx=%d", CONFIG_CSYN_NATIVE_UDP_RX_PORT,
		    CONFIG_CSYN_NATIVE_UDP_TX_PORT);
#elif defined(CONFIG_CSYN_ZENOH)
	shell_print(sh, "transport: zenoh %s %s", CONFIG_CSYN_ZENOH_MODE,
		    CONFIG_CSYN_ZENOH_LOCATOR);
#else
	shell_print(sh, "transport: not compiled");
#endif

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_csyn_topic,
	SHELL_CMD_ARG(list, NULL,
		      "list configured topics: csyn topic list [live] [filter]; 'live' shows "
		      "topics with samples only, filter matches key or name substrings",
		      cmd_csyn_topic_list, 1, 2),
	SHELL_CMD_ARG(info, &sub_csyn_topic_names, "show topic info: csyn topic info <name>",
		      cmd_csyn_topic_info, 2, 0),
	SHELL_CMD_ARG(echo, &sub_csyn_topic_names,
		      "echo topic once or periodically: csyn topic echo <name> [period_ms]",
		      cmd_csyn_topic_echo, 2, 1),
	SHELL_CMD_ARG(hz, &sub_csyn_topic_names,
		      "measure topic rate: csyn topic hz <name> [window_ms]", cmd_csyn_topic_hz, 2,
		      1),
	SHELL_CMD_ARG(watch, &sub_csyn_topic_names,
		      "watch topic on one updating line: csyn topic watch <name> [period_ms]",
		      cmd_csyn_topic_watch, 2, 1),
	SHELL_CMD(stop, NULL, "stop csyn topic echo/hz/watch", cmd_csyn_topic_stop),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_csyn,
			       SHELL_CMD(status, NULL, "show csyn status", cmd_csyn_status),
			       SHELL_CMD(topic, &sub_csyn_topic, "csyn topic commands", NULL),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(csyn, &sub_csyn, "csyn synapse topic commands", NULL);
