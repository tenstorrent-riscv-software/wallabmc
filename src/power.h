/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __POWER_H__
#define __POWER_H__

int power_init(void);
int reset_init(void);
int power_auto_poweron(void);

/*
 * Host power operations. Each holds the host power lock for its whole
 * duration on the caller's thread (on a momentary-button board a press
 * is 200 ms, a force-off 6 s, a power cycle up to ~8 s) and returns
 * -EBUSY without touching the host when another operation is in
 * progress: tell the operator to retry rather than queue. Other
 * failures return -1. power_reset() returns -ENOTSUP when the board has
 * power GPIOs but no reset line so callers can fall back to
 * power_cycle().
 */
int power_set_state(bool on);
int power_cycle(void);
int power_reset(void);
int power_force_off(void);

bool power_get_state(void);

#endif
