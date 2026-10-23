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

/*
 * What the BMC knows about host power: 1 on, 0 off, negative unknown
 * (-ENODATA on a momentary-button board with no host-power-good input,
 * otherwise the -errno of a failed read of that input). Level-driven
 * boards report the level they drive, inactive at every BMC boot;
 * momentary-button boards report the host-power-good input when the
 * board wires one. Callers treat every negative value as unknown.
 * Lock-free.
 */
int host_power_state(void);

#endif
