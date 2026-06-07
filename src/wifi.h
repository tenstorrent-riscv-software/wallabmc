/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __WIFI_H__
#define __WIFI_H__
#ifdef CONFIG_APP_WIFI
int wifi_init(void);
void wifi_exit(void);
int wifi_connect(const char *ssid, const char *password);
void wifi_disconnect(void);
#else
static inline int wifi_init(void) { return 0; }
static inline void wifi_exit(void) { }
#endif
#endif
