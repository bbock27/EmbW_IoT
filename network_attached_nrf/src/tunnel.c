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
 * BRIDGE_RELAY_HOST is an IPv4 literal parsed via zsock_inet_pton.
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

/* Read exactly `len` bytes; loops over short reads. Returns 0 or -errno. */
static int recv_all(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;
	while (len > 0) {
		int n = zsock_recv(fd, p, len, 0);
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

/* Open + connect + HELLO to (host, port). Returns the connected fd, or a
 * negative errno. Used for both the direct path and the relay fallback. */
static int do_dial(const char *host, uint16_t port)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
	};

	if (zsock_inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		LOG_ERR("'%s' is not a valid IPv4 literal", host);
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

static inline bool direct_configured(void)
{
	return CONFIG_BRIDGE_DIRECT_HOST[0] != '\0';
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

enum tunnel_path { PATH_DIRECT, PATH_RELAY };

static const char *path_name(enum tunnel_path p)
{
	return p == PATH_DIRECT ? "direct" : "relay";
}

/* Try the direct path first if configured, then fall back to relay.
 * Sets *path_out to whichever succeeded. Returns the connected fd, or
 * a negative errno if both failed. */
static int dial_with_fallback(enum tunnel_path *path_out)
{
	if (direct_configured()) {
		LOG_INF("dialing direct %s:%d",
			CONFIG_BRIDGE_DIRECT_HOST, CONFIG_BRIDGE_DIRECT_PORT);
		int fd = do_dial(CONFIG_BRIDGE_DIRECT_HOST, CONFIG_BRIDGE_DIRECT_PORT);
		if (fd >= 0) {
			*path_out = PATH_DIRECT;
			return fd;
		}
		LOG_INF("direct dial failed: %d, falling back to relay", fd);
	}

	LOG_INF("dialing relay %s:%d",
		CONFIG_BRIDGE_RELAY_HOST, CONFIG_BRIDGE_RELAY_PORT);
	int fd = do_dial(CONFIG_BRIDGE_RELAY_HOST, CONFIG_BRIDGE_RELAY_PORT);
	if (fd >= 0) {
		*path_out = PATH_RELAY;
		return fd;
	}
	return fd;
}

static void connection_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	LOG_INF("tunnel connection thread started");
	uint32_t backoff_ms = RECONNECT_BACKOFF_MIN_MS;

	while (1) {
		enum tunnel_path active;
		int fd = dial_with_fallback(&active);
		if (fd < 0) {
			LOG_WRN("all dials failed: %d, retrying in %u ms",
				fd, backoff_ms);
			k_sleep(K_MSEC(backoff_ms));
			backoff_ms = MIN(backoff_ms * 2, RECONNECT_BACKOFF_MAX_MS);
			continue;
		}

		backoff_ms = RECONNECT_BACKOFF_MIN_MS;
		int orphan;
		k_mutex_lock(&fd_mutex, K_FOREVER);
		orphan = sock_fd;
		sock_fd = fd;
		k_mutex_unlock(&fd_mutex);
		if (orphan >= 0) {
			/* Inner loop broke with a still-live fd (rare: probe-success
			 * raced with mark_dead). Close it so it doesn't leak. */
			zsock_close(orphan);
		}
		LOG_INF("tunnel up via %s (fd=%d, tunnel_id='%s')",
			path_name(active), fd, CONFIG_BRIDGE_TUNNEL_ID);

		/* Inner loop: while the current connection is alive, either wait
		 * forever (on direct, or on relay with no direct configured) or
		 * wake periodically to re-probe direct. */
		while (1) {
			k_timeout_t wait = K_FOREVER;
			if (active == PATH_RELAY && direct_configured()) {
				wait = K_SECONDS(CONFIG_BRIDGE_DIRECT_PROBE_PERIOD_S);
			}

			if (k_sem_take(&reconnect_sem, wait) == 0) {
				/* Connection died (mark_dead signalled). Re-dial from
				 * scratch -- direct first, then relay. */
				LOG_INF("tunnel: %s path dropped, reconnecting",
					path_name(active));
				break;
			}

			/* Timeout fired: we're on relay, try a direct probe. If it
			 * connects + HELLO, swap fds and continue on direct. */
			LOG_DBG("re-probing direct path");
			int probe_fd = do_dial(CONFIG_BRIDGE_DIRECT_HOST,
					       CONFIG_BRIDGE_DIRECT_PORT);
			if (probe_fd < 0) {
				LOG_DBG("direct re-probe failed: %d", probe_fd);
				continue;
			}

			LOG_INF("direct path is up; switching from relay");
			int old_fd;
			k_mutex_lock(&fd_mutex, K_FOREVER);
			old_fd = sock_fd;
			sock_fd = probe_fd;
			k_mutex_unlock(&fd_mutex);
			if (old_fd >= 0) {
				zsock_close(old_fd);
			}
			active = PATH_DIRECT;
		}
	}
}

static void receive_thread_fn(void * a, void * b, void * c) {
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	LOG_INF("tunnel_to_radio_thread started");
	while (1) {
		struct bridge_frame pkt;
		int ret = receive_tunnel_packet(&pkt, K_FOREVER);
		if (ret) {
			/* -ENOTCONN / -EPROTO / -ECONNRESET: connection thread will
			 * reconnect; back off briefly to avoid spinning. */
			k_sleep(K_MSEC(100));
			continue;
		}

		/* Record before TX so the radio echo / peer rebroadcast can be
		 * suppressed even if it arrives before transmit_802_15_4 returns. */
		bridge_dedupe_remember(pkt.data, pkt.len);

		ret = transmit_802_15_4(&pkt);
		if (ret) {
			LOG_DBG("transmit_802_15_4: %d (frame dropped)", ret);
		}
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
	ARG_UNUSED(timeout);

	if (!pkt) {
		return -EINVAL;
	}

	int fd;
	k_mutex_lock(&fd_mutex, K_FOREVER);
	fd = sock_fd;
	k_mutex_unlock(&fd_mutex);
	if (fd < 0) {
		return -ENOTCONN;
	}

	struct tunnel_hdr hdr;
	int ret = recv_all(fd, &hdr, sizeof(hdr));
	if (ret < 0) {
		LOG_DBG("recv hdr failed: %d, marking fd dead", ret);
		mark_dead(fd);
		return ret;
	}

	if (hdr.magic != TUNNEL_MAGIC || hdr.version != TUNNEL_VERSION) {
		LOG_WRN("bad header magic=%#x ver=%u, closing", hdr.magic, hdr.version);
		mark_dead(fd);
		return -EPROTO;
	}
	if (hdr.len > BRIDGE_FRAME_MAX_LEN) {
		LOG_WRN("oversized frame len=%u, closing", hdr.len);
		mark_dead(fd);
		return -EPROTO;
	}

	if (hdr.msg_type != MSG_FRAME) {
		LOG_WRN("unexpected msg_type=%u, draining %u bytes",
			hdr.msg_type, hdr.len);
		if (hdr.len > 0) {
			uint8_t scratch[BRIDGE_FRAME_MAX_LEN];
			ret = recv_all(fd, scratch, hdr.len);
			if (ret < 0) {
				mark_dead(fd);
				return ret;
			}
		}
		return -EAGAIN;
	}

	pkt->len = hdr.len;
	pkt->rssi = hdr.rssi;
	pkt->lqi = hdr.lqi;
	if (hdr.len > 0) {
		ret = recv_all(fd, pkt->data, hdr.len);
		if (ret < 0) {
			LOG_DBG("recv body failed: %d, marking fd dead", ret);
			mark_dead(fd);
			return ret;
		}
	}

	return 0;
}
