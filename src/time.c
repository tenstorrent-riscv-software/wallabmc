/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
LOG_MODULE_REGISTER(wallabmc_time, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/sys/timeutil.h>

#include "time.h"
#include "rtc.h"

static int time_iso_to_ts(const char *str, struct timespec *ts)
{
	struct tm tm;
	int year, month, day, hour, minute, second, frac, ms;
	time_t epoch_sec;
	int ret;

	memset(&tm, 0, sizeof(struct tm));

	ret = sscanf(str, "%d-%d-%dT%d:%d:%d.%3dZ",
			&year, &month, &day, &hour, &minute, &second, &frac);
	if (ret != 7)
		return -EINVAL;
	if (year < 1900)
		return -EINVAL;
	if (month < 1)
		return -EINVAL;

	tm.tm_year = year - 1900;
	tm.tm_mon  = month - 1;
	tm.tm_mday = day;
	tm.tm_hour = hour;
	tm.tm_min  = minute;
	tm.tm_sec  = second;

	epoch_sec = timeutil_timegm(&tm);
	if (epoch_sec == -1)
		return -EINVAL;

	if (frac < 10)
		ms = frac * 100;
	else if (frac < 100)
		ms = frac * 10;
	else
		ms = frac;

	ts->tv_sec = epoch_sec;
	ts->tv_nsec = (int64_t)ms * NSEC_PER_MSEC;

	return 0;
}

int time_set_from_iso_str(const char *str)
{
	static struct timespec ts;
	int ret;

	ret = time_iso_to_ts(str, &ts);
	if (ret != 0) {
		LOG_ERR("time_iso_to_ts failed (err=%d)", ret);
		return ret;
	}

	ret = rtc_set_from_timespec(&ts);
	if (ret != 0) {
		if (ret != -ENODEV)
			LOG_ERR("Updating RTC failed (err=%d)", ret);
		ret = 0; /* Don't fail for any RTC errors */
	}

	ret = sys_clock_settime(SYS_CLOCK_REALTIME, &ts);
	if (ret != 0) {
		LOG_ERR("sys_clock_settime failed (err=%d)", ret);
		return ret;
	}

	return 0;
}
