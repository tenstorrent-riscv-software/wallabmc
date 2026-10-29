/* Networking DHCPv4 client */

/*
 * Copyright (c) 2017 ARM Ltd.
 * Copyright (c) 2016 Intel Corporation.
 *
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wallabmc_dhcp, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/linker/sections.h>
#include <errno.h>
#include <stdio.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_context.h>
#include <zephyr/net/net_mgmt.h>

#include "dhcp.h"
#include "config.h"

#define DHCP_OPTION_NTP (42)

static uint8_t ntp_server[4];

static struct net_mgmt_event_callback mgmt_cb;

static struct net_dhcpv4_option_callback dhcp_cb;

/*
 * Serialises starting and stopping the client with the deferred restart
 * below. config_bmc_use_dhcp4_set() clears the setting before it takes
 * this lock to stop the client, so a deferred restart that still sees the
 * setting on is ordered before that stop, and one that sees it off does
 * nothing.
 */
static K_MUTEX_DEFINE(dhcp4_lock);

static void start_dhcpv4_client(struct net_if *iface, void *user_data)
{
	ARG_UNUSED(user_data);

	LOG_INF("Start on %s[%d]", net_if_get_device(iface)->name,
		net_if_get_by_iface(iface));
	net_dhcpv4_start(iface);
}

static void restart_dhcpv4_client(struct net_if *iface, void *user_data)
{
	ARG_UNUSED(user_data);

	LOG_INF("Restart on %s[%d]", net_if_get_device(iface)->name,
		net_if_get_by_iface(iface));
	net_dhcpv4_restart(iface);
}

static void stop_dhcpv4_client(struct net_if *iface, void *user_data)
{
	ARG_UNUSED(user_data);

	LOG_INF("Stop on %s[%d]", net_if_get_device(iface)->name,
		net_if_get_by_iface(iface));
	net_dhcpv4_stop(iface);
}

static void handler(struct net_mgmt_event_callback *cb,
		    uint64_t mgmt_event,
		    struct net_if *iface)
{
	int i = 0;

	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}

	for (i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		char buf[NET_IPV4_ADDR_LEN];

		if (iface->config.ip.ipv4->unicast[i].ipv4.addr_type !=
							NET_ADDR_DHCP) {
			continue;
		}

		LOG_INF("New lease on %s[%d]", net_if_get_device(iface)->name,
						net_if_get_by_iface(iface));
		LOG_INF("    Address: %s", net_addr_ntop(AF_INET,
				&iface->config.ip.ipv4->unicast[i].ipv4.address.in_addr,
					buf, sizeof(buf)));
		LOG_DBG("    Subnet: %s", net_addr_ntop(AF_INET,
					&iface->config.ip.ipv4->unicast[i].netmask,
					buf, sizeof(buf)));
		LOG_DBG("    Router: %s", net_addr_ntop(AF_INET,
					&iface->config.ip.ipv4->gw,
					buf, sizeof(buf)));
		LOG_DBG("    Lease time: %u seconds",
					iface->config.dhcpv4.lease_time);
	}
}

static void option_handler(struct net_dhcpv4_option_callback *cb,
			   size_t length,
			   enum net_dhcpv4_msg_type msg_type,
			   struct net_if *iface)
{
	char buf[NET_IPV4_ADDR_LEN];

	LOG_DBG("DHCP Option %d: %s", cb->option,
		net_addr_ntop(AF_INET, cb->data, buf, sizeof(buf)));
}

int dhcp4_init(void)
{
	net_mgmt_init_event_callback(&mgmt_cb, handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&mgmt_cb);

	net_dhcpv4_init_option_callback(&dhcp_cb, option_handler,
					DHCP_OPTION_NTP, ntp_server,
					sizeof(ntp_server));

	net_dhcpv4_add_option_callback(&dhcp_cb);

	return 0;
}

int start_dhcp4(void)
{
	k_mutex_lock(&dhcp4_lock, K_FOREVER);
	net_if_foreach(start_dhcpv4_client, NULL);
	k_mutex_unlock(&dhcp4_lock);

	return 0;
}

int restart_dhcp4(void)
{
	k_mutex_lock(&dhcp4_lock, K_FOREVER);
	net_if_foreach(restart_dhcpv4_client, NULL);
	k_mutex_unlock(&dhcp4_lock);

	return 0;
}

/*
 * Runs from the system work queue. The Wi-Fi connect-result handler that
 * queues it is called with the net_mgmt callback lock held, and the DHCP
 * client takes that lock under its own lock when its first interface
 * starts or its last one stops (every start and stop here), so restarting
 * from the handler could deadlock against a concurrent start or stop from
 * the shell or Redfish.
 */
static void restart_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&dhcp4_lock, K_FOREVER);
	/* The setting may have changed since the restart was queued. */
	if (config_bmc_use_dhcp4())
		net_if_foreach(restart_dhcpv4_client, NULL);
	k_mutex_unlock(&dhcp4_lock);
}

static K_WORK_DEFINE(restart_work, restart_work_fn);

int restart_dhcp4_deferred(void)
{
	k_work_submit(&restart_work);

	return 0;
}

int stop_dhcp4(void)
{
	k_mutex_lock(&dhcp4_lock, K_FOREVER);
	net_if_foreach(stop_dhcpv4_client, NULL);
	k_mutex_unlock(&dhcp4_lock);

	return 0;
}
