/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __POWER_H__
#define __POWER_H__

#include <errno.h>

int power_init(void);
int reset_init(void);
int status_led_init(void);

int power_set_state(bool on);
bool power_get_state(void);
int power_reset(void);

#ifdef CONFIG_SOM_PROTOCOL
int power_graceful_off(void);
int power_graceful_restart(void);
#else
static inline int power_graceful_off(void) { return -ENOTSUP; }
static inline int power_graceful_restart(void) { return -ENOTSUP; }
#endif

#endif
