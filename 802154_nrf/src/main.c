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

#include "radio_154.c"

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>

#ifdef CONFIG_WIFI_READY_LIB
#include <net/wifi_ready.h>
#endif

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

int main(void) {
	radio_154_init();
}
