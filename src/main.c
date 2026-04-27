/*
Adapted from the nrf-802154-recv example
*/

#include <stdio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
	LOG_MODULE_REGISTER(recv);

#include "nrf_802154.h"

#define PSDU_MAX_SIZE (127u)

// calls thi when a packet is recieved
static void send() {

}

static int rf_setup() {
	LOG_INF("RF setup started");
	printk("RF setup started\n");

	/* nrf radio driver initialization */
	nrf_802154_init();
	return 0;
}

void nrf_802154_received_raw(uint8_t *data, int8_t power, uint8_t lqi) {
	send();
	nrf_802154_buffer_free_raw(data);
}

int main(void) {
	nrf_802154_channel_set(11u);
	nrf_802154_auto_ack_set(true);
	LOG_DBG("channel: %u", nrf_802154_channel_get());

	// set the pan_id (2 bytes, little-endian)
	uint8_t pan_id[] = {0xcd, 0xab};
	nrf_802154_pan_id_set(pan_id);

	// set the extended address (8 bytes, little-endian)
	uint8_t extended_addr[] = {0x50, 0xbe, 0xca, 0xc3, 0x3c, 0x36, 0xce, 0xf4};
	nrf_802154_extended_address_set(extended_addr);

	if(nrf_802154_receive()) {
		LOG_INF("radio entered rx state");
	} else {
		LOG_ERR("driver could not enter the receive state");
	}

	return 0;
}

SYS_INIT(rf_setup, POST_KERNEL, CONFIG_PTT_RF_INIT_PRIORITY);