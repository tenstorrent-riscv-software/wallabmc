/*
 * SPDX-FileCopyrightText: © 2026 Red Hat, LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <string.h>

static uint32_t vendor_crc32(const uint8_t *data, size_t len)
{
	return crc32_ieee_update(0xFFFFFFFF, data, len);
}

#include "board_identity.h"

LOG_MODULE_REGISTER(board_identity, LOG_LEVEL_INF);

static const struct device *eeprom_dev = DEVICE_DT_GET(DT_NODELABEL(eeprom));

#define I2C_MUX_EN_NODE DT_ALIAS(i2c_mux_en)
#if DT_NODE_EXISTS(I2C_MUX_EN_NODE)
static const struct gpio_dt_spec i2c_mux_gpio =
	GPIO_DT_SPEC_GET(I2C_MUX_EN_NODE, gpios);
#endif

#define EEPROM_WP_NODE DT_ALIAS(eeprom_wp)
#if DT_NODE_EXISTS(EEPROM_WP_NODE)
static const struct gpio_dt_spec eeprom_wp_gpio =
	GPIO_DT_SPEC_GET(EEPROM_WP_NODE, gpios);
#endif

static struct carrier_board_info cbinfo;
static bool cbinfo_valid;

const struct carrier_board_info *board_identity_get(void)
{
	return cbinfo_valid ? &cbinfo : NULL;
}

const uint8_t *board_identity_mac(void)
{
	return cbinfo_valid ? cbinfo.mac_mcu : NULL;
}

const char *board_identity_serial(void)
{
	static char serial_str[BOARD_SERIAL_LEN + 1];

	if (!cbinfo_valid) {
		return "unknown";
	}

	memcpy(serial_str, cbinfo.serial_number, BOARD_SERIAL_LEN);
	serial_str[BOARD_SERIAL_LEN] = '\0';
	return serial_str;
}

static int cmd_board_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!cbinfo_valid) {
		shell_print(sh, "Retrying EEPROM read...");
		int rc = board_identity_init();
		if (rc < 0) {
			shell_error(sh, "Board identity not available (err %d)", rc);
			return rc;
		}
	}

	shell_print(sh, "--- Carrier Board Info ---");
	shell_print(sh, "Serial: %s", board_identity_serial());
	shell_print(sh, "Product ID: 0x%04x", cbinfo.product_id);
	shell_print(sh, "PCB rev: %d, BOM rev: %d, BOM variant: %d",
		    cbinfo.pcb_revision, cbinfo.bom_revision,
		    cbinfo.bom_variant);
	shell_print(sh, "MCU MAC: %02x:%02x:%02x:%02x:%02x:%02x",
		    cbinfo.mac_mcu[0], cbinfo.mac_mcu[1], cbinfo.mac_mcu[2],
		    cbinfo.mac_mcu[3], cbinfo.mac_mcu[4], cbinfo.mac_mcu[5]);
	shell_print(sh, "SOM MAC0: %02x:%02x:%02x:%02x:%02x:%02x",
		    cbinfo.mac_som0[0], cbinfo.mac_som0[1], cbinfo.mac_som0[2],
		    cbinfo.mac_som0[3], cbinfo.mac_som0[4], cbinfo.mac_som0[5]);
	shell_print(sh, "SOM MAC1: %02x:%02x:%02x:%02x:%02x:%02x",
		    cbinfo.mac_som1[0], cbinfo.mac_som1[1], cbinfo.mac_som1[2],
		    cbinfo.mac_som1[3], cbinfo.mac_som1[4], cbinfo.mac_som1[5]);

	return 0;
}

SHELL_CMD_REGISTER(board_info, NULL, "Show carrier board identity from EEPROM",
		   cmd_board_info);

int board_identity_init(void)
{
	int ret;

#if DT_NODE_EXISTS(I2C_MUX_EN_NODE)
	if (gpio_is_ready_dt(&i2c_mux_gpio)) {
		gpio_pin_configure_dt(&i2c_mux_gpio, GPIO_OUTPUT_ACTIVE);
		gpio_pin_set_dt(&i2c_mux_gpio, 1);
		k_msleep(10);
	} else {
		LOG_WRN("I2C mux GPIO not ready");
	}
#endif

	if (!device_is_ready(eeprom_dev)) {
		ret = device_init(eeprom_dev);
		if (ret < 0) {
			LOG_ERR("EEPROM device init failed: %d", ret);
			return ret;
		}
	}

#if DT_NODE_EXISTS(EEPROM_WP_NODE)
	if (gpio_is_ready_dt(&eeprom_wp_gpio)) {
		gpio_pin_configure_dt(&eeprom_wp_gpio, GPIO_OUTPUT_ACTIVE);
		gpio_pin_set_dt(&eeprom_wp_gpio, 1);
	}
#endif

	ret = eeprom_read(eeprom_dev, 0, &cbinfo, sizeof(cbinfo));
	if (ret < 0) {
		LOG_ERR("Failed to read EEPROM: %d", ret);
		return ret;
	}

	if (cbinfo.magic != CBINFO_MAGIC) {
		LOG_WRN("EEPROM magic mismatch: 0x%08x (expected 0x%08x)",
			cbinfo.magic, CBINFO_MAGIC);
		cbinfo_valid = false;
		return -EINVAL;
	}

	size_t crc_len = offsetof(struct carrier_board_info, crc32);

	uint32_t calc_crc = vendor_crc32((const uint8_t *)&cbinfo, crc_len);

	if (calc_crc != cbinfo.crc32) {
		LOG_WRN("EEPROM CRC mismatch: calc=0x%08x stored=0x%08x",
			calc_crc, cbinfo.crc32);
		ret = eeprom_read(eeprom_dev, 80, &cbinfo, sizeof(cbinfo));
		if (ret < 0 || cbinfo.magic != CBINFO_MAGIC) {
			LOG_ERR("Backup EEPROM also invalid");
			cbinfo_valid = false;
			return -EINVAL;
		}
		calc_crc = vendor_crc32((const uint8_t *)&cbinfo, crc_len);
		if (calc_crc != cbinfo.crc32) {
			LOG_ERR("Backup EEPROM CRC also invalid");
			cbinfo_valid = false;
			return -EINVAL;
		}
		LOG_INF("Using backup EEPROM data");
	}

	cbinfo_valid = true;
	LOG_INF("Board serial: %s", board_identity_serial());
	LOG_INF("MCU MAC: %02x:%02x:%02x:%02x:%02x:%02x",
		cbinfo.mac_mcu[0], cbinfo.mac_mcu[1], cbinfo.mac_mcu[2],
		cbinfo.mac_mcu[3], cbinfo.mac_mcu[4], cbinfo.mac_mcu[5]);

	return 0;
}
