/*
 * SPDX-FileCopyrightText: © 2026 Red Hat, LLC
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __BOARD_IDENTITY_H__
#define __BOARD_IDENTITY_H__

#include <stdint.h>

#define BOARD_SERIAL_LEN 18

struct carrier_board_info {
	uint32_t magic;
	uint8_t  format_version;
	uint16_t product_id;
	uint8_t  pcb_revision;
	uint8_t  bom_revision;
	uint8_t  bom_variant;
	char     serial_number[BOARD_SERIAL_LEN];
	uint8_t  mfg_test_status;
	uint8_t  mac_som0[6];
	uint8_t  mac_som1[6];
	uint8_t  mac_mcu[6];
	uint32_t crc32;
} __packed;

#define CBINFO_MAGIC 0x45505EF1

#ifdef CONFIG_BOARD_IDENTITY
int board_identity_init(void);
const struct carrier_board_info *board_identity_get(void);
const uint8_t *board_identity_mac(void);
const char *board_identity_serial(void);
#else
static inline int board_identity_init(void) { return 0; }
static inline const uint8_t *board_identity_mac(void) { return NULL; }
static inline const char *board_identity_serial(void) { return "unknown"; }
#endif

#endif
