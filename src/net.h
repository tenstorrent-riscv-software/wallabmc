/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __NET_H__
#define __NET_H__

#include <stdbool.h>

int net_init(void);
void net_exit(void);

/* A single DNS label, as used for the BMC's hostname */
bool net_hostname_valid(const char *hostname);
int net_do_set_hostname(const char *hostname);
int net_do_set_default_ip4_from_config(void);
int net_start_dhcp4(void);
int net_stop_dhcp4(void);

#endif
