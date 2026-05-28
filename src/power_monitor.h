/*
 * SPDX-FileCopyrightText: © 2026 Red Hat, LLC
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __POWER_MONITOR_H__
#define __POWER_MONITOR_H__

#ifdef CONFIG_POWER_MONITOR
int power_monitor_read(int32_t *voltage_mv, int32_t *current_ma,
		       int32_t *power_mw);
#else
static inline int power_monitor_read(int32_t *v, int32_t *c, int32_t *p)
{
	*v = 0; *c = 0; *p = 0;
	return -ENOTSUP;
}
#endif

#endif
