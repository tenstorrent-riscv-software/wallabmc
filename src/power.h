/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __POWER_H__
#define __POWER_H__

int power_init(void);
int reset_init(void);
int power_auto_poweron(void);

int power_set_state(bool on);
bool power_get_state(void);
int power_cycle(void);
int power_reset(void);
int power_force_off(void);

#endif
