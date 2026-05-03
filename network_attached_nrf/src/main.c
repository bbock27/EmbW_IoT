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

#include "bridge.h"

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

#define WIFI_SHELL_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | \
				NET_EVENT_WIFI_DISCONNECT_RESULT)

#define STATUS_POLLING_MS  300
#define LED_SLEEP_TIME_MS  100

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static struct net_mgmt_event_callback wifi_mgmt_cb;
static struct net_mgmt_event_callback net_mgmt_cb;

#ifdef CONFIG_WIFI_READY_LIB
static K_SEM_DEFINE(wifi_ready_state_changed_sem, 0, 1);
static bool wifi_ready_status;
#endif

static struct {
	uint8_t connected            : 1;
	uint8_t connect_result       : 1;
	uint8_t disconnect_requested : 1;
	uint8_t _unused              : 5;
} context;

static void toggle_led(void)
{
	if (!device_is_ready(led.port)) {
		LOG_ERR("LED device not ready");
		return;
	}
	if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE) < 0) {
		LOG_ERR("Failed to configure LED pin");
		return;
	}
	while (1) {
		if (context.connected) {
			gpio_pin_toggle_dt(&led);
		} else {
			gpio_pin_set_dt(&led, 0);
		}
		k_msleep(LED_SLEEP_TIME_MS);
	}
}
K_THREAD_DEFINE(led_thread_id, 1024, toggle_led, NULL, NULL, NULL, 7, 0, 0);

static int log_wifi_status(void)
{
	struct net_if *iface = net_if_get_default();
	struct wifi_iface_status status = { 0 };

	if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status, sizeof(status))) {
		LOG_INF("WiFi status request failed");
		return -ENOEXEC;
	}

	LOG_INF("==================");
	LOG_INF("State: %s", wifi_state_txt(status.state));
	if (status.state >= WIFI_STATE_ASSOCIATED) {
		LOG_INF("Mode: %s", wifi_mode_txt(status.iface_mode));
		LOG_INF("Link Mode: %s", wifi_link_mode_txt(status.link_mode));
		LOG_INF("SSID: %.32s", status.ssid);
		LOG_INF("Band: %s", wifi_band_txt(status.band));
		LOG_INF("Channel: %d", status.channel);
		LOG_INF("Security: %s", wifi_security_txt(status.security));
		LOG_INF("MFP: %s", wifi_mfp_txt(status.mfp));
		LOG_INF("RSSI: %d", status.rssi);
	}
	return 0;
}

static void handle_wifi_connect_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	if (context.connected) {
		return;
	}
	if (status->status) {
		LOG_ERR("WiFi connection failed (%d)", status->status);
	} else {
		LOG_INF("WiFi connected");
		context.connected = true;
	}
	context.connect_result = true;
}

static void handle_wifi_disconnect_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	if (!context.connected) {
		return;
	}
	if (context.disconnect_requested) {
		LOG_INF("WiFi disconnect %s (%d)",
			status->status ? "failed" : "done", status->status);
		context.disconnect_requested = false;
	} else {
		LOG_INF("WiFi disconnected");
		context.connected = false;
	}
	log_wifi_status();
}

static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				    uint64_t mgmt_event,
				    struct net_if *iface)
{
	ARG_UNUSED(iface);
	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		handle_wifi_connect_result(cb);
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		handle_wifi_disconnect_result(cb);
		break;
	default:
		break;
	}
}

static void on_dhcp_bound(struct net_mgmt_event_callback *cb)
{
	const struct net_if_dhcpv4 *dhcpv4 = cb->info;
	char addr_str[NET_IPV4_ADDR_LEN];

	net_addr_ntop(AF_INET, &dhcpv4->requested_ip, addr_str, sizeof(addr_str));
	LOG_INF("DHCP bound: %s", addr_str);

	int ret = bridge_start();
	if (ret) {
		LOG_ERR("bridge_start returned %d", ret);
	}
}

static void net_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				   uint64_t mgmt_event,
				   struct net_if *iface)
{
	ARG_UNUSED(iface);
	if (mgmt_event == NET_EVENT_IPV4_DHCP_BOUND) {
		on_dhcp_bound(cb);
	}
}

static int wifi_connect(void)
{
	struct net_if *iface = net_if_get_first_wifi();

	context.connected = false;
	context.connect_result = false;

	if (net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, iface, NULL, 0)) {
		LOG_ERR("WiFi connect request failed");
		return -ENOEXEC;
	}
	LOG_INF("WiFi connect requested");
	return 0;
}

static int start_app(void)
{
	while (1) {
#ifdef CONFIG_WIFI_READY_LIB
		LOG_INF("Waiting for WiFi to be ready");
		int ret = k_sem_take(&wifi_ready_state_changed_sem, K_FOREVER);
		if (ret) {
			LOG_ERR("Failed to take WiFi-ready semaphore: %d", ret);
			return ret;
		}
check_wifi_ready:
		if (!wifi_ready_status) {
			LOG_INF("WiFi is not ready");
			continue;
		}
#endif

		wifi_connect();
		while (!context.connect_result) {
			log_wifi_status();
			k_sleep(K_MSEC(STATUS_POLLING_MS));
		}

		if (context.connected) {
			log_wifi_status();
#ifdef CONFIG_WIFI_READY_LIB
			ret = k_sem_take(&wifi_ready_state_changed_sem, K_FOREVER);
			if (ret) {
				LOG_ERR("Failed to take WiFi-ready semaphore: %d", ret);
				return ret;
			}
			goto check_wifi_ready;
#else
			k_sleep(K_FOREVER);
#endif
		}
	}
	return 0;
}

#ifdef CONFIG_WIFI_READY_LIB
static void start_wifi_thread_fn(void);
#define START_WIFI_THREAD_PRIO K_PRIO_COOP(CONFIG_NUM_COOP_PRIORITIES - 1)
K_THREAD_DEFINE(start_wifi_thread_id,
		CONFIG_BRIDGE_WIFI_START_STACK_SIZE,
		start_wifi_thread_fn, NULL, NULL, NULL,
		START_WIFI_THREAD_PRIO, 0, -1);

static void start_wifi_thread_fn(void)
{
	start_app();
}

static void wifi_ready_cb(bool ready)
{
	LOG_DBG("WiFi ready? %s", ready ? "yes" : "no");
	wifi_ready_status = ready;
	k_sem_give(&wifi_ready_state_changed_sem);
}

static int register_wifi_ready_(void)
{
	wifi_ready_callback_t cb = { .wifi_ready_cb = wifi_ready_cb };
	struct net_if *iface = net_if_get_first_wifi();

	if (!iface) {
		LOG_ERR("No WiFi interface");
		return -1;
	}
	int ret = register_wifi_ready_callback(cb, iface);
	if (ret) {
		LOG_ERR("Failed to register WiFi-ready callback: %s", strerror(ret));
	}
	return ret;
}
#endif

static void mgmt_callbacks_init(void)
{
	net_mgmt_init_event_callback(&wifi_mgmt_cb, wifi_mgmt_event_handler,
				     WIFI_SHELL_MGMT_EVENTS);
	net_mgmt_add_event_callback(&wifi_mgmt_cb);

	net_mgmt_init_event_callback(&net_mgmt_cb, net_mgmt_event_handler,
				     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&net_mgmt_cb);

	LOG_INF("Starting %s with CPU frequency: %d MHz", CONFIG_BOARD,
		SystemCoreClock / MHZ(1));
	k_sleep(K_SECONDS(1));
}

int main(void)
{
	mgmt_callbacks_init();

#ifdef CONFIG_WIFI_READY_LIB
	int ret = register_wifi_ready_();
	if (ret) {
		return ret;
	}
	k_thread_start(start_wifi_thread_id);
#else
	start_app();
#endif
	return 0;
}
