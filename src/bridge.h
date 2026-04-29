/*
 * 802.15.4 <-> WiFi tunnel bridge -- shared types and prototypes.
 */

#ifndef BRIDGE_H_
#define BRIDGE_H_

#include <stdint.h>
#include <zephyr/kernel.h>

#define BRIDGE_FRAME_MAX_LEN 127

struct bridge_frame {
	uint8_t len;                              /* PHY payload length, <= BRIDGE_FRAME_MAX_LEN */
	int8_t  rssi;                             /* valid on receive paths only */
	uint8_t lqi;                              /* valid on receive paths only */
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

/* Start the bridge: initializes the radio + tunnel modules and spawns the two glue threads.
 * Safe to call once after WiFi+DHCP are up. Subsequent calls are no-ops.
 */
int bridge_start(void);

#endif /* BRIDGE_H_ */
