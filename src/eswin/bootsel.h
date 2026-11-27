/*
 * SPDX-FileCopyrightText: © 2026 Red Hat, LLC
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __BOOTSEL_H__
#define __BOOTSEL_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef CONFIG_BOOTSEL
int bootsel_init(void);
int bootsel_read(uint8_t *value);
int bootsel_set_sw_mode(uint8_t value);
int bootsel_set_hw_mode(void);
bool bootsel_is_sw_mode(void);
const char *bootsel_boot_source(uint8_t sel);
#else
static inline int bootsel_init(void) { return 0; }
static inline int bootsel_read(uint8_t *value) { *value = 0; return 0; }
static inline int bootsel_set_sw_mode(uint8_t value) { return -ENOTSUP; }
static inline int bootsel_set_hw_mode(void) { return -ENOTSUP; }
static inline bool bootsel_is_sw_mode(void) { return false; }
static inline const char *bootsel_boot_source(uint8_t sel) { return "Unknown"; }
#endif

#endif
