/*
 * SPDX-FileCopyrightText: © 2026 Red Hat, LLC
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __FAN_H__
#define __FAN_H__

#ifdef CONFIG_FAN_CONTROL
int fan_init(void);
int fan_set_duty(int fan_num, int duty_pct);
int fan_get_duty(int fan_num);
int fan_get_rpm(int fan_num);
#else
static inline int fan_init(void) { return 0; }
static inline int fan_set_duty(int fan_num, int duty_pct) { return -1; }
static inline int fan_get_duty(int fan_num) { return -1; }
static inline int fan_get_rpm(int fan_num) { return -1; }
#endif

#endif
