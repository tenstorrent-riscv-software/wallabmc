/*
 * Based on samples/net/sockets/sntp_client/src/main.c
 * Copyright (c) 2017 Linaro Limited
 * Copyright (c) 2019 Intel Corporation
 *
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wallabmc_ntp, LOG_LEVEL_DBG);

#include <zephyr/net/socket.h>
#include <zephyr/net/sntp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "ntp.h"
#include "rtc.h"
#include "config.h"
#include "workq.h"

static int sntp_set_clocks(struct sntp_time *ts)
{
	struct timespec tspec;
	int ret;

	tspec.tv_sec = ts->seconds;
	tspec.tv_nsec = ((uint64_t)ts->fraction * NSEC_PER_SEC) >> 32;

	ret = rtc_set_from_timespec(&tspec);
	if (ret != 0) {
		if (ret != -ENODEV)
			LOG_ERR("Updating RTC failed (err=%d)", ret);
		ret = 0; /* Don't fail for any RTC errors */
	}

	ret = sys_clock_settime(SYS_CLOCK_REALTIME, &tspec);
	if (ret < 0) {
		LOG_ERR("Setting sys clock failed (err=%d)", ret);
		return ret;
	}

	LOG_INF("Time synced to NTP");

	return ret;
}

static int dns_query(const char *host, uint16_t port, int family, int socktype,
			struct sockaddr *addr, socklen_t *addrlen)
{
	struct addrinfo hints = {
		.ai_family = family,
		.ai_socktype = socktype,
	};
	struct addrinfo *res = NULL;
	int rv;

	rv = getaddrinfo(host, NULL, &hints, &res);
	if (rv < 0) {
		LOG_ERR("getaddrinfo failed (%d, errno %d)", rv, errno);
		return rv;
	}

	*addr = *res->ai_addr;
	*addrlen = res->ai_addrlen;

	freeaddrinfo(res);

	net_sin(addr)->sin_port = htons(port);

	return 0;
}

#define NTP_SERVER_PORT 123

static int do_sntp(int family)
{
	char *family_str = family == AF_INET ? "IPv4" : "IPv6";
	struct sntp_time s_time;
	struct sntp_ctx ctx;
	struct sockaddr addr;
	socklen_t addrlen;
	int rv;
	const char *server = config_bmc_ntp_server();

	/* Get SNTP server */
	rv = dns_query(server, NTP_SERVER_PORT, family, SOCK_DGRAM, &addr, &addrlen);
	if (rv != 0) {
		LOG_ERR("Failed to lookup %s SNTP server (err=%d)", family_str, rv);
		return rv;
	}

	rv = sntp_init(&ctx, &addr, addrlen);
	if (rv < 0) {
		LOG_ERR("Failed to init SNTP %s ctx (err=%d)", family_str, rv);
		goto err;
	}

	rv = sntp_query(&ctx, 4 * MSEC_PER_SEC, &s_time);
	if (rv < 0) {
		LOG_ERR("SNTP %s request failed (err=%d)", family_str, rv);
		goto err;
	}

	sntp_close(&ctx);

	rv = sntp_set_clocks(&s_time);
	if (rv < 0) {
		LOG_WRN("SNTP Could not set clock (err=%d)", rv);
		return rv;
	}

	return 0;

err:
	sntp_close(&ctx);

	return rv;
}

static bool ntp_synced;
static bool ntp_started;
static uint32_t ntp_interval_sec = 20; /* Initially 20 second sync so the network can come up */

/*
 * A sync blocks in getaddrinfo() and sntp_query() for up to about 6 s,
 * so it runs on the blocking workqueue. On the system workqueue it
 * would hold up every other item there, the DNS resolver's own query
 * timeout included.
 */
static void ntp_sync_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(ntp_sync_work, ntp_sync_work_handler);

static void ntp_sync_work_handler(struct k_work *work)
{
	ntp_interval_sec = 60; /* Upgrade to 1 minute sync until synced */
	if (do_sntp(AF_INET) == 0) {
		if (!ntp_synced) {
			ntp_synced = true;
			/* Upgrade to 6 hour resync */
			ntp_interval_sec = 6*60*60;
		}
	}

	k_work_schedule_for_queue(&blocking_work_q, &ntp_sync_work, K_SECONDS(ntp_interval_sec));
}

bool ntp_is_synced(void)
{
	return ntp_synced;
}

int start_ntp(void)
{
	if (ntp_started)
		return 0;

	k_work_schedule_for_queue(&blocking_work_q, &ntp_sync_work, K_SECONDS(ntp_interval_sec));

	ntp_started = true;

	return 0;
}

int stop_ntp(void)
{
	struct k_work_sync work_sync;

	if (!ntp_started)
		return 0;

	/* Waits out an in-flight attempt; it cannot re-arm after this */
	k_work_cancel_delayable_sync(&ntp_sync_work, &work_sync);

	ntp_interval_sec = 1; /* Wait 1 second if we start again */

	ntp_started = false;

	return 0;
}

int ntp_init(void)
{
	if (config_bmc_use_ntp())
		return start_ntp();

	return 0;
}
