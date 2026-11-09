/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int config_init(void);
int config_clear(void);
const char *config_bmc_hostname(void);
int config_bmc_hostname_set(const char *hostname);
int config_bmc_password_set(const char *password);
bool config_bmc_use_ntp(void);
int config_bmc_use_ntp_set(bool use);
const char *config_bmc_ntp_server(void);
int config_bmc_ntp_server_set(const char *server);
uint32_t config_bmc_default_ip4(void);
int config_bmc_default_ip4_set(const char *str);
uint32_t config_bmc_default_ip4_nm(void);
int config_bmc_default_ip4_nm_set(const char *str);
uint32_t config_bmc_default_ip4_gw(void);
int config_bmc_default_ip4_gw_set(const char *str);
bool config_bmc_use_dhcp4(void);
int config_bmc_use_dhcp4_set(bool use);
bool config_host_auto_poweron(void);
int config_host_auto_poweron_set(bool on);
const char *config_bmc_admin_password(void);
bool config_bmc_https_psk(const char **psk, int *psk_len);
bool config_bmc_wifi_enable(void);
const char *config_bmc_wifi_ssid(void);
const char *config_bmc_wifi_password(void);

/* True if config is saved to persistent storage */
bool config_persistent(void);

/* Binary config items, read and written whole */
enum config_blob {
	CONFIG_BLOB_HTTPS_KEY,
	CONFIG_BLOB_HTTPS_CERT,
	CONFIG_BLOB_HTTPS_CERT_SOURCE,
};

/*
 * Returns the item's length, or a negative error: -ENOENT if it isn't
 * stored, -EINVAL if it's larger than size, -ENODEV without persistent
 * storage.
 */
ssize_t config_blob_read(enum config_blob blob, void *buf, size_t size);
int config_blob_write(enum config_blob blob, const void *buf, size_t size);
int config_blob_delete(enum config_blob blob);
#endif
