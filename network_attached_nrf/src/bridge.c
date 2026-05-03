/*
 * Bridge orchestrator -- wires radio_154 and tunnel together.
 *
 * Spawns radio_to_tunnel_thread, the simple loop that composes
 * receive_802_15_4 + tunnel_packet. The complementary
 * tunnel_to_radio_thread plus dedupe ring lands in step 5.
 */

#include "bridge.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(bridge, CONFIG_LOG_DEFAULT_LEVEL);

#define RADIO_TO_TUNNEL_STACK_SIZE 3072

#define DEDUPE_RING_DEPTH 8
#define DEDUPE_TTL_MS     500

struct dedupe_entry {
	uint32_t timestamp;
	uint8_t  len;
	uint8_t  data[BRIDGE_FRAME_MAX_LEN];
};

static struct dedupe_entry dedupe_ring[DEDUPE_RING_DEPTH];
static int dedupe_idx;
static K_MUTEX_DEFINE(dedupe_mutex);

void bridge_dedupe_remember(const uint8_t *data, uint8_t len)
{
	if (!data || len == 0 || len > BRIDGE_FRAME_MAX_LEN) {
		return;
	}
	k_mutex_lock(&dedupe_mutex, K_FOREVER);
	struct dedupe_entry *e = &dedupe_ring[dedupe_idx];
	e->timestamp = k_uptime_get_32();
	e->len = len;
	memcpy(e->data, data, len);
	dedupe_idx = (dedupe_idx + 1) % DEDUPE_RING_DEPTH;
	k_mutex_unlock(&dedupe_mutex);
}

bool bridge_dedupe_seen(const uint8_t *data, uint8_t len)
{
	if (!data || len == 0 || len > BRIDGE_FRAME_MAX_LEN) {
		return false;
	}
	bool seen = false;
	uint32_t now = k_uptime_get_32();
	k_mutex_lock(&dedupe_mutex, K_FOREVER);
	for (int i = 0; i < DEDUPE_RING_DEPTH; i++) {
		const struct dedupe_entry *e = &dedupe_ring[i];
		if (e->len == len &&
		    (now - e->timestamp) < DEDUPE_TTL_MS &&
		    memcmp(e->data, data, len) == 0) {
			seen = true;
			break;
		}
	}
	k_mutex_unlock(&dedupe_mutex);
	return seen;
}

static atomic_t bridge_started;

static K_THREAD_STACK_DEFINE(radio_to_tunnel_stack, RADIO_TO_TUNNEL_STACK_SIZE);
static struct k_thread radio_to_tunnel_thread_data;

static void radio_to_tunnel_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	LOG_INF("radio_to_tunnel_thread started");
	struct bridge_frame f;

	while (1) {
		int ret = receive_802_15_4(&f, K_FOREVER);
		if (ret) {
			LOG_DBG("receive_802_15_4: %d", ret);
			k_sleep(K_MSEC(100));  /* avoid tight loop on persistent error */
			continue;
		}

		if (bridge_dedupe_seen(f.data, f.len)) {
			LOG_DBG("dedupe: dropping echo (len=%u)", f.len);
			continue;
		}

		ret = tunnel_packet(&f);
		if (ret) {
			LOG_DBG("tunnel_packet: %d (frame dropped)", ret);
		}
	}
}

int bridge_start(void)
{
	if (atomic_set(&bridge_started, 1) == 1) {
		LOG_DBG("bridge already started, ignoring re-entry");
		return 0;
	}

	LOG_INF("bridge_start: relay=%s:%d tunnel_id='%s' channel=%d pan=0x%04x",
		CONFIG_BRIDGE_RELAY_HOST, CONFIG_BRIDGE_RELAY_PORT,
		CONFIG_BRIDGE_TUNNEL_ID, CONFIG_BRIDGE_15_4_CHANNEL,
		CONFIG_BRIDGE_15_4_PAN_ID);

	int ret = radio_154_init();
	if (ret && ret != -ENOSYS) {
		LOG_ERR("radio_154_init failed: %d (continuing)", ret);
	}

	ret = tunnel_init();
	if (ret) {
		LOG_ERR("tunnel_init failed: %d", ret);
		return ret;
	}

	k_thread_create(&radio_to_tunnel_thread_data, radio_to_tunnel_stack,
			K_THREAD_STACK_SIZEOF(radio_to_tunnel_stack),
			radio_to_tunnel_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	k_thread_name_set(&radio_to_tunnel_thread_data, "radio2tunnel");

	LOG_INF("bridge: radio_to_tunnel_thread up; tunnel_to_radio + dedupe still TODO (step 5)");
	return 0;
}
