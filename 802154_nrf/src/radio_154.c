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


#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "nrf_802154.h"

#define TX_DONE_TIMEOUT_MS 500

LOG_MODULE_REGISTER(radio_154, CONFIG_LOG_DEFAULT_LEVEL);

/* RX slab: backing storage for received bridge_frame instances.
 * Block size is rounded up to a 4-byte multiple as K_MEM_SLAB_DEFINE
 * requires alignment-multiple block size. */
#define MAX_PAYLOAD_SIZE    102
#define MAX_PACKET_SIZE	    127

/* RX msgq: holds bridge_frame * pointers (into rx_slab). */
K_MSGQ_DEFINE(rx_msgq, sizeof(struct bridge_frame *),
	      CONFIG_BRIDGE_RX_SLAB_DEPTH, sizeof(void *));

/* TX is one-in-flight: transmit_802_15_4 blocks on tx_done_sem until the
 * driver invokes transmitted_raw or transmit_failed. The radio retains
 * the buffer pointer until then, so the caller's bridge_frame must stay
 * valid for the duration of the call. */
static K_SEM_DEFINE(tx_done_sem, 0, 1);

static uint8_t src_extended_addr[8] = {0xde, 0xad, 0xbe, 0xef, 0xc0, 0xff, 0xee, 0xaa};
static uint8_t dest_extended_addr[8] = {0x50, 0xbe, 0xca, 0xc3, 0x3c, 0x36, 0xce, 0xf4};
uint8_t src_pan_id[2] = {0xcf, 0xab};


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

	 
	uint8_t pdsu_len = data[0];
	if (pdsu_len == 0 || pdsu_len > MAX_PACKET_SIZE) {
		return;
	}

	LOG_INF("Received packet: %s", (char *) (data + 30));

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

	nrf_802154_extended_address_set(src_extended_addr);

	if (nrf_802154_receive()) {
		LOG_INF("radio entered rx state");
		return 0;
	}
	LOG_ERR("driver could not enter the receive state");
	return -EIO;
}

void nrf_802154_transmitted_raw(uint8_t *p_frame,
	const nrf_802154_transmit_done_metadata_t *p_metadata)
{
	ARG_UNUSED(p_frame);
	ARG_UNUSED(p_metadata);
	k_sem_give(&tx_done_sem);
}

void nrf_802154_transmit_failed(uint8_t *p_frame,
	nrf_802154_tx_error_t error,
	const nrf_802154_transmit_done_metadata_t *p_metadata)
{
	ARG_UNUSED(p_frame);
	ARG_UNUSED(p_metadata);
	LOG_WRN("nrf_802154 TX failed: %u", (unsigned)error);
	k_sem_give(&tx_done_sem);
}

int transmit_802_15_4(uint8_t * buf, size_t len)
{
	if (len > MAX_PAYLOAD_SIZE) {
		LOG_ERR("payload size is too large to transmit");
		return -E2BIG;
	}

	size_t packet_len = 30 + 2 + len;

	uint8_t pkt[MAX_PACKET_SIZE];
	pkt[0] = packet_len; /* Length for nrf_transmit (length of pkt + FCS) */
	pkt[1] = 0x11; /* Frame Control Field */
	pkt[2] = 0x88; /* Frame Control Field */
	pkt[3] = 0x00; /* Sequence number */
	pkt[4] = 0xbb; /* Destination PAN ID 0xffff */
	pkt[5] = 0xbb; /* Destination PAN ID */
	memcpy(&pkt[6], dest_extended_addr, 8); /* Destination extended address */
	memcpy(&pkt[14], src_pan_id, 2); /* Source PAN ID */
	memcpy(&pkt[22], src_extended_addr, 8);/* Source extended address */
	memcpy(&pkt[30], buf, len);

	const nrf_802154_transmit_metadata_t metadata = {
		.frame_props = NRF_802154_TRANSMITTED_FRAME_PROPS_DEFAULT_INIT,
		.cca = true,
	};

	/* bridge_frame is laid out so &pkt->len is the contiguous [PHR][PSDU...]
	 * the radio expects. The driver borrows the buffer until transmitted_raw
	 * or transmit_failed fires, so block here until the callback. */
	k_sem_reset(&tx_done_sem);
	nrf_802154_tx_error_t err = nrf_802154_transmit_raw((uint8_t *)&packet_len, &metadata);
	if (err != NRF_802154_TX_ERROR_NONE) {
		LOG_DBG("transmit_raw rejected: 0x%x", (unsigned)err);
		return -EIO;
	}
	if (k_sem_take(&tx_done_sem, K_MSEC(TX_DONE_TIMEOUT_MS)) != 0) {
		LOG_WRN("transmit_raw: TX-done timeout");
		return -ETIMEDOUT;
	}
	return 0;
}
