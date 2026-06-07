/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __WIFI_H__
#define __WIFI_H__
#ifdef CONFIG_APP_WIFI
int wifi_connect_init(void);
#else
static inline int wifi_connect_init(void) { return 0; }
#endif
#endif
