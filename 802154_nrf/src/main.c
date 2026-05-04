#include <stdio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/logging/log.h>
	LOG_MODULE_REGISTER(send);

#include "nrf_802154.h"
#include <inttypes.h>

#define PSDU_MAX_SIZE (127u)
#define FCS_LENGTH (2u) /* Length of the Frame Control Sequence */

#define SW0_NODE	DT_ALIAS(sw0)
#if !DT_NODE_HAS_STATUS_OKAY(SW0_NODE)
#error "Unsupported board: sw0 devicetree alias is not defined"
#endif
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(SW0_NODE, gpios,
							      {0});
static struct gpio_callback button_cb_data;

#define MESSAGE_SIZE 24
static char send_message[MESSAGE_SIZE] = "sending data from nrf 1";
static uint8_t src_pan_id[2] = {CONFIG_BRIDGE_15_4_PAN_ID & 0xff, (CONFIG_BRIDGE_15_4_PAN_ID >> 8) & 0xff};
static uint8_t src_extended_addr[] = {0xdc, 0xa9, 0x35, 0x7b, 0x73, 0x36, 0xce, 0xf4};
static uint8_t dst_extended_addr[] = {0x50, 0xbe, 0xca, 0xc3, 0x3c, 0x36, 0xce, 0xf4};

static uint8_t pkt[PSDU_MAX_SIZE];


static void pkt_hexdump(uint8_t *pkt, uint8_t length) {
  int i;
  printk("Packet: ");
	for (i=0; i<length; i++) {
		printk("%02x ", *pkt);
		pkt++;
	}
	printk("\n");
}

static void cast_string(uint8_t *pkt, uint8_t length) {
    printk(&pkt[23]);
}

void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
	size_t len = 24 + FCS_LENGTH + MESSAGE_SIZE;
	// send a tx pkt
	pkt[0] = len; /* Length for nrf_transmit (length of pkt + FCS) */
	pkt[1] = 0x01; /* Frame Control Field */
	pkt[2] = 0xcc; /* Frame Control Field */
	pkt[3] = 0x00; /* Sequence number */
	pkt[4] = src_pan_id[0]; /* Destination PAN ID 0xffff */
	pkt[5] = src_pan_id[1]; /* Destination PAN ID */
	memcpy(&pkt[6], dst_extended_addr, 8); /* Destination extended address */
	memcpy(&pkt[14], src_pan_id, 2); /* Source PAN ID */
	memcpy(&pkt[16], src_extended_addr, 8);/* Source extended address */
	memcpy(&pkt[24], send_message, MESSAGE_SIZE);

	const nrf_802154_transmit_metadata_t metadata = {
			.frame_props = NRF_802154_TRANSMITTED_FRAME_PROPS_DEFAULT_INIT,
			.cca = true
		};

	int err;

	err = nrf_802154_transmit_raw(pkt, &metadata);

	if(err) {
		LOG_ERR("driver could not schedule the transmission procedure. Reason Ox%x", err);
		printk("Reason Ox%x", err);
	}
}

static int rf_setup() {
	LOG_INF("RF setup started");
	// ARG_UNUSED(dev);

	/* nrf radio driver initialization */
	nrf_802154_init();

	return 0;
}

// callback fn when tx starts
void nrf_802154_tx_started(const uint8_t *p_frame) {
	LOG_INF("tx started");
}

// callback fn when tx fails
void nrf_802154_transmit_failed(uint8_t *frame, nrf_802154_tx_error_t error,
	const nrf_802154_transmit_done_metadata_t *p_metadata) {
	LOG_INF("tx failed error %u!", error);
}

// callback fn for successful tx
void nrf_802154_transmitted_raw(uint8_t *p_frame,
	const nrf_802154_transmit_done_metadata_t *p_metadata) {
	LOG_INF("frame was transmitted!");
}

int main(int argc, char **argv) {
	nrf_802154_channel_set(CONFIG_BRIDGE_15_4_CHANNEL);
	LOG_DBG("channel: %u", nrf_802154_channel_get());

	// set the pan_id (2 bytes, little-endian)
	nrf_802154_pan_id_set(src_pan_id);

	// set the extended address (8 bytes, little-endian)
	nrf_802154_extended_address_set(src_extended_addr);

		int ret;

	if (!gpio_is_ready_dt(&button)) {
		printk("Error: button device %s is not ready\n",
		       button.port->name);
		return 0;
	}

	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret != 0) {
		printk("Error %d: failed to configure %s pin %d\n",
		       ret, button.port->name, button.pin);
		return 0;
	}

	ret = gpio_pin_interrupt_configure_dt(&button,
					      GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		printk("Error %d: failed to configure interrupt on %s pin %d\n",
			ret, button.port->name, button.pin);
		return 0;
	}

	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);
	printk("Set up button at %s pin %d\n", button.port->name, button.pin);

	if (nrf_802154_receive()) {
		LOG_INF("radio entered rx state");
		return 0;
	}
	
	return 0;
}


void nrf_802154_received_raw(uint8_t *data, int8_t power, uint8_t lqi) {
    printk("received:");
	// pkt_hexdump(data+1, *data - 2U); /* print packet from byte [1, len-2] */
    cast_string(data+1, *data - 2U);
	nrf_802154_buffer_free_raw(data);
}


SYS_INIT(rf_setup, POST_KERNEL, 90);