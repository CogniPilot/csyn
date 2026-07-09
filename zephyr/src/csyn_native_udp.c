/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <csyn/csyn.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(csyn_udp, LOG_LEVEL_INF);

/*
 * Frame: "CSYN" magic, little-endian u16 synapse catalog topic id,
 * little-endian u16 payload length, payload bytes.
 */
#define CSYN_UDP_MAGIC_0     'C'
#define CSYN_UDP_MAGIC_1     'S'
#define CSYN_UDP_MAGIC_2     'Y'
#define CSYN_UDP_MAGIC_3     'N'
#define CSYN_UDP_HEADER_SIZE 8U
#define CSYN_UDP_MAX_TOPICS  32U

static int g_rx_sock = -1;
static int g_tx_sock = -1;
static struct sockaddr_in g_tx_addr;
static K_THREAD_STACK_DEFINE(g_csyn_udp_stack, CONFIG_CSYN_NATIVE_UDP_THREAD_STACK_SIZE);
static struct k_thread g_csyn_udp_thread;

static uint16_t get_le16(const uint8_t *buf)
{
	return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static void put_le16(uint16_t value, uint8_t *buf)
{
	buf[0] = (uint8_t)(value & 0xffU);
	buf[1] = (uint8_t)((value >> 8) & 0xffU);
}

static int socket_set_nonblocking(int sock)
{
	int flags = fcntl(sock, F_GETFL, 0);

	if (flags < 0) {
		return -errno;
	}

	if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
		return -errno;
	}

	return 0;
}

static int socket_init(int *sock, uint16_t bind_port)
{
	struct sockaddr_in addr = {0};
	int rc;

	*sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (*sock < 0) {
		return -errno;
	}

	rc = socket_set_nonblocking(*sock);
	if (rc != 0) {
		close(*sock);
		*sock = -1;
		return rc;
	}

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(bind_port);

	if (bind_port != 0U && bind(*sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		rc = -errno;
		close(*sock);
		*sock = -1;
		return rc;
	}

	return 0;
}

static int destination_init(struct sockaddr_in *addr)
{
	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_port = htons(CONFIG_CSYN_NATIVE_UDP_TX_PORT);

	if (inet_pton(AF_INET, CONFIG_CSYN_NATIVE_UDP_HOST, &addr->sin_addr) != 1) {
		return -EINVAL;
	}

	return 0;
}

static void rx_drain(void)
{
	uint8_t buf[CSYN_UDP_HEADER_SIZE + CONFIG_CSYN_FLATBUFFER_MAX_SIZE];
	static bool logged_external_odometry_rx;
	static bool logged_unknown_rx;

	while (true) {
		ssize_t len = recv(g_rx_sock, buf, sizeof(buf), 0);
		struct csyn_topic *topic;
		uint16_t payload_len;
		uint16_t catalog_id;

		if (len < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				LOG_WRN("csyn udp rx failed: %d", errno);
			}
			return;
		}

		if ((size_t)len < CSYN_UDP_HEADER_SIZE || buf[0] != CSYN_UDP_MAGIC_0 ||
		    buf[1] != CSYN_UDP_MAGIC_1 || buf[2] != CSYN_UDP_MAGIC_2 ||
		    buf[3] != CSYN_UDP_MAGIC_3) {
			continue;
		}

		catalog_id = get_le16(buf + 4U);
		payload_len = get_le16(buf + 6U);
		if ((size_t)payload_len + CSYN_UDP_HEADER_SIZE != (size_t)len) {
			continue;
		}

		topic = csyn_topic_by_catalog_id(catalog_id);
		if (topic == NULL) {
			if (!logged_unknown_rx) {
				LOG_WRN("csyn udp unknown topic id=%u payload_len=%u",
					(unsigned int)catalog_id, (unsigned int)payload_len);
				logged_unknown_rx = true;
			}
			continue;
		}

		if (!logged_external_odometry_rx &&
		    strcmp(topic->key_suffix, "external_odometry") == 0) {
			LOG_INF("csyn udp external_odometry rx id=%u payload_len=%u",
				(unsigned int)catalog_id, (unsigned int)payload_len);
			logged_external_odometry_rx = true;
		}
		(void)csyn_topic_publish(topic, buf + CSYN_UDP_HEADER_SIZE, payload_len);
	}
}

static void tx_topic_if_updated(struct csyn_topic *topic, uint32_t *last_generation)
{
	uint8_t buf[CSYN_UDP_HEADER_SIZE + CONFIG_CSYN_FLATBUFFER_MAX_SIZE];
	size_t len;
	uint32_t generation = csyn_topic_generation(topic);

	if (topic->info == NULL || generation == 0U || generation == *last_generation) {
		return;
	}

	if (!csyn_topic_copy(topic, buf + CSYN_UDP_HEADER_SIZE, sizeof(buf) - CSYN_UDP_HEADER_SIZE,
			     &len, NULL)) {
		return;
	}

	buf[0] = CSYN_UDP_MAGIC_0;
	buf[1] = CSYN_UDP_MAGIC_1;
	buf[2] = CSYN_UDP_MAGIC_2;
	buf[3] = CSYN_UDP_MAGIC_3;
	put_le16(topic->info->id, buf + 4U);
	put_le16((uint16_t)len, buf + 6U);

	(void)sendto(g_tx_sock, buf, len + CSYN_UDP_HEADER_SIZE, 0, (struct sockaddr *)&g_tx_addr,
		     sizeof(g_tx_addr));
	*last_generation = generation;
}

static void csyn_udp_thread(void *arg0, void *arg1, void *arg2)
{
	static uint32_t last_generation[CSYN_UDP_MAX_TOPICS];

	ARG_UNUSED(arg0);
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);

	if (csyn_topic_count() > CSYN_UDP_MAX_TOPICS) {
		LOG_ERR("too many csyn topics for udp transport");
		return;
	}

	while (true) {
		rx_drain();
		for (size_t i = 0U; i < csyn_topic_count(); i++) {
			struct csyn_topic *topic = csyn_topic_at(i);

			if (topic->dir == CSYN_DIR_TX) {
				tx_topic_if_updated(topic, &last_generation[i]);
			}
		}
		k_sleep(K_MSEC(1));
	}
}

static int csyn_native_udp_init(void)
{
	int rc;

	rc = socket_init(&g_rx_sock, CONFIG_CSYN_NATIVE_UDP_RX_PORT);
	if (rc != 0) {
		LOG_ERR("csyn udp rx socket init failed: %d", -rc);
		return rc;
	}

	rc = socket_init(&g_tx_sock, 0U);
	if (rc != 0) {
		LOG_ERR("csyn udp tx socket init failed: %d", -rc);
		close(g_rx_sock);
		g_rx_sock = -1;
		return rc;
	}

	rc = destination_init(&g_tx_addr);
	if (rc != 0) {
		LOG_ERR("csyn udp destination init failed");
		return rc;
	}

	k_thread_create(&g_csyn_udp_thread, g_csyn_udp_stack,
			K_THREAD_STACK_SIZEOF(g_csyn_udp_stack), csyn_udp_thread, NULL, NULL, NULL,
			CONFIG_CSYN_NATIVE_UDP_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&g_csyn_udp_thread, "csyn_udp");

	LOG_INF("csyn udp rx=%d tx=%d host=%s", CONFIG_CSYN_NATIVE_UDP_RX_PORT,
		CONFIG_CSYN_NATIVE_UDP_TX_PORT, CONFIG_CSYN_NATIVE_UDP_HOST);

	return 0;
}

SYS_INIT(csyn_native_udp_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
