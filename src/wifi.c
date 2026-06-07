/* Wi-Fi station bring-up using compile-time credentials. */

/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wallabmc_wifi, LOG_LEVEL_INF);

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/shell/shell.h>

#include "config.h"
#include "wifi.h"

#define WIFI_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | \
			  NET_EVENT_WIFI_DISCONNECT_RESULT)

static struct net_mgmt_event_callback wifi_cb;
static K_SEM_DEFINE(wifi_disconnected_sem, 0, 1);
static bool wifi_connecting;
static bool wifi_connected;

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event,
			       struct net_if *iface)
{
	const struct wifi_status *status = cb->info;

	ARG_UNUSED(iface);

	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		if (status && status->status) {
			LOG_ERR("Wi-Fi connect failed (status=%d)", status->status);
		} else {
			LOG_INF("Wi-Fi connected");
			wifi_connected = true;
		}
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		if (wifi_connected) {
			/* ESP gives disconnected events even when not connected. */
			LOG_INF("Wi-Fi disconnected");
			wifi_connected = false;
		}
		k_sem_give(&wifi_disconnected_sem);
		break;
	default:
		break;
	}
}

/*
 * Connect using a runtime-supplied SSID/password pair.
 *
 * Avoid wifi_credentials_set_personal() + NET_REQUEST_WIFI_CONNECT_STORED
 * here because that path also re-adds the static entry whenever the
 * stored SSID does not match it, which races with our update on the
 * ESP32-C6 build and intermittently wedges Wi-Fi association after a
 * disconnect+reconnect.
 */
static int wifi_connect_with(struct net_if *iface, const char *ssid, const char *password)
{
	static uint8_t ssid_buf[WIFI_SSID_MAX_LEN + 1];
	static uint8_t psk_buf[WIFI_PSK_MAX_LEN + 1];
	static struct wifi_connect_req_params params = {
		.ssid = ssid_buf,
		.psk = psk_buf,
	};
	size_t ssid_len = strlen(ssid);
	size_t psk_len = strlen(password);

	if (ssid_len == 0 || ssid_len > WIFI_SSID_MAX_LEN)
		return -EINVAL;
	if (psk_len > WIFI_PSK_MAX_LEN)
		return -EINVAL;

	memcpy(ssid_buf, ssid, ssid_len);
	params.ssid_length = ssid_len;
	memcpy(psk_buf, password, psk_len);
	params.psk_length = psk_len;
	params.security = (psk_len > 0) ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE;
	params.channel = WIFI_CHANNEL_ANY;
	params.band = WIFI_FREQ_BAND_UNKNOWN;
	params.mfp = WIFI_MFP_OPTIONAL;
	params.timeout = SYS_FOREVER_MS;

	return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
}

int wifi_connect(const char *ssid, const char *password)
{
	struct net_if *iface = net_if_get_first_wifi();
	int rc;

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	if (ssid[0] == '\0') {
		LOG_WRN("No Wi-Fi credentials configured -- use `config bmc wifi ssid <ssid>` and `config bmc wifi password <password>` to set them");
		return 0;
	}

	if (wifi_connecting)
		wifi_disconnect();
	wifi_connecting = true;

	LOG_INF("Connecting to Wi-Fi SSID: %s", ssid);
	rc = wifi_connect_with(iface, ssid, password);
	if (rc) {
		LOG_ERR("Wi-Fi connect request failed (rc=%d)", rc);
		return rc;
	}
	LOG_INF("Wi-Fi connect requested; waiting for association...");

	return 0;
}

void wifi_disconnect(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	int rc;

	/*
	 * Disassociate from the current AP (best effort -- ignore -EALREADY)
	 * and wait briefly for the driver to confirm before reconnecting
	 * with the new credentials.
	 */
	k_sem_reset(&wifi_disconnected_sem);
	rc = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	if (rc && rc != -EALREADY) {
		LOG_WRN("Wi-Fi disconnect request failed (rc=%d)", rc);
	} else {
		wifi_connecting = false;
		if (rc == 0) {
			if (k_sem_take(&wifi_disconnected_sem, K_MSEC(1000)))
				LOG_WRN("Wi-Fi disconnect timeout");
			k_msleep(200);
		}
	}
}

int wifi_init(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	int rc;

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	k_sem_reset(&wifi_disconnected_sem);

	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
				     WIFI_MGMT_EVENTS);
	net_mgmt_add_event_callback(&wifi_cb);

	if (config_bmc_wifi_enable())
		return wifi_connect(config_bmc_wifi_ssid(), config_bmc_wifi_password());

	return 0;
}
