/*
 * IEEE 802.15.4 driver wrapper.
 *
 * Radio bring-up: driver init via SYS_INIT, then channel / auto-ACK /
 * PAN ID / extended address set in radio_154_init(), then enter RX.
 *
 * RX path: nrf_802154_received_raw allocates a bridge_frame from
 * rx_slab, copies the PSDU bytes (and rssi/lqi) in, and pushes a
 * pointer onto rx_msgq. receive_802_15_4 drains the msgq, copies the
 * frame out for the caller, and frees the slab entry.
 *
 * transmit_802_15_4 is still -ENOSYS (later step).
 */

#include "bridge.h"

#include <errno.h>
#include <stdint.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "nrf_802154.h"

LOG_MODULE_REGISTER(radio_154, CONFIG_LOG_DEFAULT_LEVEL);

/* RX slab: backing storage for received bridge_frame instances.
 * Block size is rounded up to a 4-byte multiple as K_MEM_SLAB_DEFINE
 * requires alignment-multiple block size. */
#define RX_SLAB_BLOCK_SIZE  ROUND_UP(sizeof(struct bridge_frame), 4)
#define RX_SLAB_ALIGN       4

K_MEM_SLAB_DEFINE(rx_slab, RX_SLAB_BLOCK_SIZE,
		  CONFIG_BRIDGE_RX_SLAB_DEPTH, RX_SLAB_ALIGN);

/* RX msgq: holds bridge_frame * pointers (into rx_slab). */
K_MSGQ_DEFINE(rx_msgq, sizeof(struct bridge_frame *),
	      CONFIG_BRIDGE_RX_SLAB_DEPTH, sizeof(void *));

static int rf_setup(void)
{
	LOG_INF("RF setup started");
	printk("RF setup started\n");
	nrf_802154_init();
	return 0;
}

SYS_INIT(rf_setup, POST_KERNEL, 90);

void nrf_802154_received_raw(uint8_t *data, int8_t power, uint8_t lqi)
{
	/* TODO: populate the RX path.
	 *
	 * nrf_802154 hands back PHR + PSDU: data[0] = PSDU length (includes
	 * the 2-byte FCS), data[1..1+data[0]] = PSDU bytes.
	 *
	 *   1. Read psdu_len = data[0]; drop if 0 or > BRIDGE_FRAME_MAX_LEN.
	 *   2. k_mem_slab_alloc(&rx_slab, &frame, K_NO_WAIT). On failure, drop.
	 *   3. frame->len = psdu_len; frame->rssi = power; frame->lqi = lqi;
	 *      memcpy(frame->data, &data[1], psdu_len).
	 *   4. nrf_802154_buffer_free_raw(data) so the radio can reuse the buffer.
	 *   5. k_msgq_put(&rx_msgq, &frame, K_NO_WAIT). If full, k_mem_slab_free.
	 *
	 * Until populated, just release the driver buffer so RX doesn't stall.
	 */
	ARG_UNUSED(power);
	ARG_UNUSED(lqi);
	nrf_802154_buffer_free_raw(data);
}

int radio_154_init(void)
{
	nrf_802154_channel_set(CONFIG_BRIDGE_15_4_CHANNEL);
	nrf_802154_auto_ack_set(true);
	LOG_DBG("channel: %u", nrf_802154_channel_get());

	/* PAN ID, little-endian wire order. */
	uint8_t pan_id[2] = {
		CONFIG_BRIDGE_15_4_PAN_ID & 0xff,
		(CONFIG_BRIDGE_15_4_PAN_ID >> 8) & 0xff,
	};
	nrf_802154_pan_id_set(pan_id);

	
	uint8_t extended_addr[8] = {
		0x50, 0xbe, 0xca, 0xc3, 0x3c, 0x36, 0xce, 0xf4,
	};
	nrf_802154_extended_address_set(extended_addr);

	if (nrf_802154_receive()) {
		LOG_INF("radio entered rx state");
		return 0;
	}
	LOG_ERR("driver could not enter the receive state");
	return -EIO;
}

int transmit_802_15_4(const struct bridge_frame *pkt)
{
	ARG_UNUSED(pkt);
	return -ENOSYS;
}

int receive_802_15_4(struct bridge_frame *pkt, k_timeout_t timeout)
{
	/* TODO: populate the consumer side.
	 *
	 *   1. Validate pkt is non-NULL (-EINVAL if not).
	 *   2. struct bridge_frame *frame;
	 *      ret = k_msgq_get(&rx_msgq, &frame, timeout);
	 *      -- returns -EAGAIN on timeout when caller passes K_NO_WAIT or K_MSEC.
	 *   3. memcpy(pkt, frame, sizeof(*pkt)).
	 *   4. k_mem_slab_free(&rx_slab, frame).
	 *   5. return 0.
	 */
	ARG_UNUSED(pkt);
	ARG_UNUSED(timeout);
	return -ENOSYS;
}

unsigned int radio_154_rx_pending(void)
{
	return k_msgq_num_used_get(&rx_msgq);
}

void radio_154_rx_purge(void)
{
	struct bridge_frame *frame;
	while (k_msgq_get(&rx_msgq, &frame, K_NO_WAIT) == 0) {
		k_mem_slab_free(&rx_slab, frame);
	}
}
