/*
 * 802.15.4 <-> WiFi tunnel bridge -- main entry.
 *
 * WiFi-bringup flow ported from /Users/brady/ncfsund/sta/src/main.c
 * with the following changes:
 *   - Dropped the QSPI encryption block (only applied to nRF7002DK).
 *   - Dropped the BSSID print in cmd_wifi_status() (removes the
 *     net_private.h private-API dependency).
 *   - bridge_start() is invoked once after DHCP is bound.
 *
 * The previous EmbW_IoT/src/main.c (a small 802.15.4 RX skeleton) is
 * preserved as comments inside src/radio_154.c -- it becomes the basis
 * of radio_154_init() in step 3.
 */

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

#include "radio_154.h"

#define SW0_NODE	DT_ALIAS(sw0)
#if !DT_NODE_HAS_STATUS_OKAY(SW0_NODE)
#error "Unsupported board: sw0 devicetree alias is not defined"
#endif
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(SW0_NODE, gpios,
							      {0});
static struct gpio_callback button_cb_data;

#define MESSAGE_SIZE 23
static char send_message[MESSAGE_SIZE] = "sending data from nrf 1";

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
	transmit_802_15_4(send_message, MESSAGE_SIZE);
}

int main(void) {
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

	radio_154_init();
	k_sleep(K_FOREVER);
}
