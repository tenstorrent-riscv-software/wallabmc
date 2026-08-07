/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __RTC_H__
#define __RTC_H__

#if defined(CONFIG_RTC)
int rtc_init(void);
int rtc_set_from_clock(void);
int time_set_from_iso_str(const char *str);
#else /* defined(CONFIG_RTC) */
static inline int rtc_init(void) { return 0; }
static inline int rtc_set_from_clock(void) { return -ENODEV; }
static inline int time_set_from_iso_str(const char *str) { return -ENODEV; }
#endif /* defined(CONFIG_RTC) */

#endif /* __RTC_H__ */
