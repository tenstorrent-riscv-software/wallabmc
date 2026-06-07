/* Wi-Fi station bring-up using compile-time credentials. */

/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wallabmc_wifi, LOG_LEVEL_INF);

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

#include "wifi.h"

#define WIFI_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | \
			  NET_EVENT_WIFI_DISCONNECT_RESULT)

static struct net_mgmt_event_callback wifi_cb;

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
		}
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		LOG_WRN("Wi-Fi disconnected");
		break;
	default:
		break;
	}
}

int wifi_connect_init(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_ps_params ps = {
		.enabled = WIFI_PS_DISABLED,
	};
	int rc;

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
				     WIFI_MGMT_EVENTS);
	net_mgmt_add_event_callback(&wifi_cb);

	/*
	 * The ESP32 driver enables WIFI_PS_MAX_MODEM by default, which causes
	 * 100+ ms RX latency spikes and dropped frames under bursty HTTP load.
	 * The BMC is mains-powered, so trade the power saving for reliability.
	 */
	rc = net_mgmt(NET_REQUEST_WIFI_PS, iface, &ps, sizeof(ps));
	if (rc) {
		LOG_WRN("Could not disable Wi-Fi power save (rc=%d)", rc);
	}

	rc = net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, iface, NULL, 0);
	if (rc) {
		LOG_ERR("Wi-Fi connect-stored request failed (rc=%d)", rc);
		return rc;
	}

	LOG_INF("Wi-Fi connect requested; waiting for association...");
	return 0;
}
