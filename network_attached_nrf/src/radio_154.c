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
	/* nrf_802154 hands back PHR + PSDU: data[0] = PSDU length (includes
	 * the 2-byte FCS), data[1..1+data[0]] = PSDU bytes.
	 * Allocate a bridge_frame, populate it, push to rx_msgq. */
	uint8_t pdsu_len = data[0];
	if (pdsu_len == 0 || pdsu_len > BRIDGE_FRAME_MAX_LEN) {
		return;
	}

	struct bridge_frame * frame;
	if (k_mem_slab_alloc(&rx_slab, (void **) &frame, K_NO_WAIT) != 0) {
		return;
	}

	frame->len = pdsu_len;
	frame->rssi = power;
	frame->lqi = lqi;
	memcpy(frame->data, data + 1, pdsu_len);

	nrf_802154_buffer_free_raw(data);

	LOG_INF("Received 802.15.4 packet");

	if (k_msgq_put(&rx_msgq, &frame, K_NO_WAIT) == 0) {
		LOG_DBG("Added message to the queue");
	} else {
		LOG_WRN("Msg queue is full");
	}
}

int radio_154_init(void)
{
	nrf_802154_channel_set(CONFIG_BRIDGE_15_4_CHANNEL);
	nrf_802154_auto_ack_set(false);
	LOG_DBG("channel: %u", nrf_802154_channel_get());

	/* PAN ID, little-endian wire order. */
	// uint8_t pan_id[2] = {
	// 	CONFIG_BRIDGE_15_4_PAN_ID & 0xff,
	// 	(CONFIG_BRIDGE_15_4_PAN_ID >> 8) & 0xff,
	// };
	// nrf_802154_pan_id_set(pan_id);

	/* Enable promiscuous mode to capture all frames on the channel,
	 * regardless of destination address or PAN ID. */
	nrf_802154_promiscuous_set(true);

	// uint8_t extended_addr[8] = {
	// 	0x50, 0xbe, 0xca, 0xc3, 0x3c, 0x36, 0xce, 0xf4,
	// };
	// nrf_802154_extended_address_set(extended_addr);

	if (nrf_802154_receive()) {
		LOG_INF("radio entered rx state");
		return 0;
	}
	LOG_ERR("driver could not enter the receive state");
	return -EIO;
}

static K_SEM_DEFINE(tx_done_sem, 0, 1);

/* TX buffer: must be persistent (static) since nrf_802154_transmit_raw uses zero-copy.
 * The driver stores a pointer to this buffer and transmits it asynchronously. */
static uint8_t tx_buf[1 + BRIDGE_FRAME_MAX_LEN];

void nrf_802154_transmitted_raw(uint8_t *p_frame,
	const nrf_802154_transmit_done_metadata_t *p_metadata) {
	ARG_UNUSED(p_frame);
	ARG_UNUSED(p_metadata);
	k_sem_give(&tx_done_sem);
}

int transmit_802_15_4(const struct bridge_frame *pkt)
{
	if (!pkt || pkt->len > BRIDGE_FRAME_MAX_LEN) {
		return -EINVAL;
	}

	/* nrf_802154_transmit_raw expects [PHR][PSDU] where PHR = length of PSDU.
	 * bridge_frame.len is the PSDU length; populate the static TX buffer. */
	tx_buf[0] = pkt->len;
	memcpy(tx_buf + 1, pkt->data, pkt->len);

	const nrf_802154_transmit_metadata_t metadata = {
		.frame_props = NRF_802154_TRANSMITTED_FRAME_PROPS_DEFAULT_INIT,
		.cca = true
	};

	nrf_802154_tx_error_t tx_error = nrf_802154_transmit_raw(tx_buf, &metadata);
	if (tx_error != NRF_802154_TX_ERROR_NONE) {
		LOG_ERR("transmit_raw failed with error code: 0x%02x", tx_error);
		return -EIO;
	}

	/* Wait for TX-done callback. Use a 1s timeout to avoid indefinite hang
	 * if the callback is somehow lost. */
	if (k_sem_take(&tx_done_sem, K_SECONDS(1)) != 0) {
		LOG_WRN("transmit_802_15_4: TX-done callback timeout");
		return -ETIMEDOUT;
	}
    LOG_INF("802.15.4 TX succeeded");


	return 0;
}

int receive_802_15_4(struct bridge_frame *pkt, k_timeout_t timeout)
{
	if (pkt == NULL) {
		return -EINVAL;
	}

	// get msg from q
	struct bridge_frame * frame;
	if(k_msgq_get(&rx_msgq, &frame, timeout) != 0) {
		return -EAGAIN;
	}

	// copy packet and free
	memcpy(pkt, frame, sizeof(*pkt));
	k_mem_slab_free(&rx_slab, frame);
	
	return 0;
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
