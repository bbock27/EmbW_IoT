/*
 * 802.15.4 <-> WiFi tunnel bridge -- shared types and prototypes.
 */

#ifndef BRIDGE_H_
#define BRIDGE_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/toolchain.h>

#define BRIDGE_FRAME_MAX_LEN 127

/* `len` immediately precedes `data` and the struct is packed so that
 * &pkt->len is the contiguous [PHR][PSDU...] buffer the nrf_802154 radio
 * driver expects for transmit_raw / received_raw. */
struct __packed bridge_frame {
	int8_t  rssi;                             /* valid on receive paths only */
	uint8_t lqi;                              /* valid on receive paths only */
	uint8_t len;                              /* PHY payload length, <= BRIDGE_FRAME_MAX_LEN */
	uint8_t data[BRIDGE_FRAME_MAX_LEN];       /* raw 802.15.4 frame bytes */
};

/* Data path: four blocking primitives, returns 0 on success or a negative errno. */

/* Send a captured 802.15.4 frame through the WiFi tunnel to the relay. */
int tunnel_packet(const struct bridge_frame *pkt);

/* Receive a frame from the relay, stripping the tunnel header. */
int receive_tunnel_packet(struct bridge_frame *pkt, k_timeout_t timeout);

/* Transmit a frame on the local 802.15.4 radio. */
int transmit_802_15_4(const struct bridge_frame *pkt);

/* Receive a frame captured by the local 802.15.4 radio. Blocks up to `timeout`. */
int receive_802_15_4(struct bridge_frame *pkt, k_timeout_t timeout);

/* Number of frames currently buffered in radio_154's RX queue. */
unsigned int radio_154_rx_pending(void);

/* Drop all currently-buffered RX frames (releases their slab entries). */
void radio_154_rx_purge(void);

/* One-time initialization helpers used by bridge_start(). */
int radio_154_init(void);
int tunnel_init(void);

/* Dedupe: bridge_dedupe_remember is called just before the bridge transmits a
 * frame on its local 802.15.4 radio. bridge_dedupe_seen is called for each
 * frame just received off the air; if it returns true the frame is an echo of
 * something we just transmitted (or a peer's echo of our TX) and should be
 * dropped instead of tunneled, breaking the loop. Entries expire on a short
 * timer, so a stale TX recording can't shadow a legitimate frame for long. */
void bridge_dedupe_remember(const uint8_t *data, uint8_t len);
bool bridge_dedupe_seen(const uint8_t *data, uint8_t len);

/* Start the bridge: initializes the radio + tunnel modules and spawns the two glue threads.
 * Safe to call once after WiFi+DHCP are up. Subsequent calls are no-ops.
 */
int bridge_start(void);

#endif /* BRIDGE_H_ */
