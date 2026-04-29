/*
 * TCP tunnel to the relay server.
 *
 * Step 4: tunnel_init() spawns a connection-management thread that
 * establishes a long-lived TCP socket to BRIDGE_RELAY_HOST:BRIDGE_RELAY_PORT,
 * sends a HELLO carrying BRIDGE_TUNNEL_ID, and reconnects with backoff
 * on failure. tunnel_packet() snapshots the fd under a mutex and writes
 * a single length-prefixed FRAME message; on send failure it marks the
 * fd dead and signals the connection thread to reconnect.
 *
 * receive_tunnel_packet() is still -ENOSYS (step 5).
 *
 * BRIDGE_RELAY_HOST is parsed as an IPv4 literal via zsock_inet_pton.
 * Hostname support would need CONFIG_DNS_RESOLVER and getaddrinfo.
 */

#include "bridge.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

LOG_MODULE_REGISTER(tunnel, CONFIG_LOG_DEFAULT_LEVEL);

#define TUNNEL_MAGIC       0x34353142u  /* 'B154' little-endian */
#define TUNNEL_VERSION     1
#define MSG_HELLO          1
#define MSG_FRAME          2
#define HELLO_PAYLOAD_LEN  16

#define RECONNECT_BACKOFF_MIN_MS  1000
#define RECONNECT_BACKOFF_MAX_MS  30000

#define CONNECTION_THREAD_STACK   4096
#define RECV_THREAD_STACK 	      4096

struct __packed tunnel_hdr {
	uint32_t magic;
	uint8_t  version;
	uint8_t  msg_type;
	uint16_t seq;
	int8_t   rssi;
	uint8_t  lqi;
	uint8_t  len;
	uint8_t  _pad;
};

BUILD_ASSERT(sizeof(struct tunnel_hdr) == 12, "tunnel_hdr must be 12 bytes");

static K_MUTEX_DEFINE(fd_mutex);
static int sock_fd = -1;            /* protected by fd_mutex */
static K_SEM_DEFINE(reconnect_sem, 0, 1);

static K_THREAD_STACK_DEFINE(connection_stack, CONNECTION_THREAD_STACK);
static struct k_thread connection_thread;

static K_THREAD_STACK_DEFINE(recv_thread_stack, RECV_THREAD_STACK);
static struct k_thread recv_thread;

static uint16_t tx_seq;              /* only touched by tunnel_packet */

/* Send `len` bytes; loops over short writes. Returns 0 or -errno. */
static int send_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len > 0) {
		int n = zsock_send(fd, p, len, 0);
		if (n < 0) {
			return -errno;
		}
		if (n == 0) {
			return -ECONNRESET;
		}
		p += n;
		len -= n;
	}
	return 0;
}

/* Open + connect + HELLO. Returns the connected fd, or a negative errno. */
static int do_connect(void)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_BRIDGE_RELAY_PORT),
	};

	if (zsock_inet_pton(AF_INET, CONFIG_BRIDGE_RELAY_HOST, &addr.sin_addr) != 1) {
		LOG_ERR("BRIDGE_RELAY_HOST='%s' is not a valid IPv4 literal "
			"(hostname support needs CONFIG_DNS_RESOLVER + getaddrinfo)",
			CONFIG_BRIDGE_RELAY_HOST);
		return -EINVAL;
	}

	int fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		return -errno;
	}

	if (zsock_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		int e = -errno;
		zsock_close(fd);
		return e;
	}

	int one = 1;
	if (zsock_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
		LOG_WRN("TCP_NODELAY failed: %d (continuing without it)", errno);
	}

	/* HELLO: header + 16-byte zero-padded ASCII tunnel_id. */
	uint8_t buf[sizeof(struct tunnel_hdr) + HELLO_PAYLOAD_LEN] = {0};
	struct tunnel_hdr hdr = {
		.magic = TUNNEL_MAGIC,
		.version = TUNNEL_VERSION,
		.msg_type = MSG_HELLO,
		.len = HELLO_PAYLOAD_LEN,
	};
	memcpy(buf, &hdr, sizeof(hdr));
	strncpy((char *)buf + sizeof(hdr), CONFIG_BRIDGE_TUNNEL_ID, HELLO_PAYLOAD_LEN);

	int ret = send_all(fd, buf, sizeof(buf));
	if (ret < 0) {
		LOG_WRN("HELLO send failed: %d", ret);
		zsock_close(fd);
		return ret;
	}

	return fd;
}

/* Close the fd if it still matches `expected_fd`, and signal a reconnect. */
static void mark_dead(int expected_fd)
{
	bool need_signal = false;
	k_mutex_lock(&fd_mutex, K_FOREVER);
	if (sock_fd >= 0 && sock_fd == expected_fd) {
		zsock_close(sock_fd);
		sock_fd = -1;
		need_signal = true;
	}
	k_mutex_unlock(&fd_mutex);
	if (need_signal) {
		k_sem_give(&reconnect_sem);
	}
}

static void connection_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	LOG_INF("tunnel connection thread started");
	uint32_t backoff_ms = RECONNECT_BACKOFF_MIN_MS;

	while (1) {
		int fd = do_connect();
		if (fd < 0) {
			LOG_WRN("connect to %s:%d failed: %d, retrying in %u ms",
				CONFIG_BRIDGE_RELAY_HOST, CONFIG_BRIDGE_RELAY_PORT,
				fd, backoff_ms);
			k_sleep(K_MSEC(backoff_ms));
			backoff_ms = MIN(backoff_ms * 2, RECONNECT_BACKOFF_MAX_MS);
			continue;
		}

		backoff_ms = RECONNECT_BACKOFF_MIN_MS;
		k_mutex_lock(&fd_mutex, K_FOREVER);
		sock_fd = fd;
		k_mutex_unlock(&fd_mutex);
		LOG_INF("tunnel connected to %s:%d (fd=%d, tunnel_id='%s')",
			CONFIG_BRIDGE_RELAY_HOST, CONFIG_BRIDGE_RELAY_PORT,
			fd, CONFIG_BRIDGE_TUNNEL_ID);

		/* Block until something marks the connection dead. */
		k_sem_take(&reconnect_sem, K_FOREVER);
		LOG_INF("tunnel: reconnecting");
	}
}

static void receive_thread_fn(void * a, void * b, void * c) {
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	while (1) {
		struct bridge_frame pkt;
		receive_tunnel_packet(&pkt, K_MSEC(10));

		k_sleep(K_MSEC(100));
	}
}

int tunnel_init(void)
{
	k_thread_create(&connection_thread, connection_stack,
			K_THREAD_STACK_SIZEOF(connection_stack),
			connection_thread_fn, NULL, NULL, NULL,
			K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	k_thread_name_set(&connection_thread, "tunnel-conn");

	k_thread_create(&recv_thread, recv_thread_stack,
			K_THREAD_STACK_SIZEOF(recv_thread_stack),
			receive_thread_fn, NULL, NULL, NULL,
			K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	k_thread_name_set(&recv_thread, "recv-tun");
	
	return 0;
}

int tunnel_packet(const struct bridge_frame *pkt)
{
	if (!pkt || pkt->len > BRIDGE_FRAME_MAX_LEN) {
		return -EINVAL;
	}

	int fd;
	k_mutex_lock(&fd_mutex, K_FOREVER);
	fd = sock_fd;
	k_mutex_unlock(&fd_mutex);

	if (fd < 0) {
		return -ENOTCONN;
	}

	/* One send: header + payload concatenated, so the relay reads a
	 * complete (hdr + len) unit even if the underlying TCP segments
	 * fragment differently. */
	uint8_t buf[sizeof(struct tunnel_hdr) + BRIDGE_FRAME_MAX_LEN];
	struct tunnel_hdr hdr = {
		.magic = TUNNEL_MAGIC,
		.version = TUNNEL_VERSION,
		.msg_type = MSG_FRAME,
		.seq = tx_seq++,
		.rssi = pkt->rssi,
		.lqi = pkt->lqi,
		.len = pkt->len,
	};
	memcpy(buf, &hdr, sizeof(hdr));
	if (pkt->len > 0) {
		memcpy(buf + sizeof(hdr), pkt->data, pkt->len);
	}

	int ret = send_all(fd, buf, sizeof(hdr) + pkt->len);
	if (ret < 0) {
		LOG_DBG("tunnel_packet send failed: %d, marking fd dead", ret);
		mark_dead(fd);
	}
	return ret;
}

int receive_tunnel_packet(struct bridge_frame *pkt, k_timeout_t timeout)
{
	// get socket fd
	int fd;
	k_mutex_lock(&fd_mutex, K_FOREVER);
	fd = sock_fd;
	k_mutex_unlock(&fd_mutex);

	if (fd < 0) {
		return -ENOTCONN;
	}

	// recv msg from socket
	uint8_t buf[sizeof(struct tunnel_hdr) + BRIDGE_FRAME_MAX_LEN];
	size_t len = zsock_recv (fd, buf, sizeof(struct tunnel_hdr) + BRIDGE_FRAME_MAX_LEN, 0);
	if (len <= sizeof(struct tunnel_hdr)) {
		return -ENOTCONN;
	}

	// populate packet
	struct tunnel_hdr header;
	memcpy(&header, buf, sizeof(header));
	memcpy(pkt, buf + sizeof(header), len - sizeof(header));

	// transmit packet over 802.15.4
	return transmit_802_15_4(pkt);
}
