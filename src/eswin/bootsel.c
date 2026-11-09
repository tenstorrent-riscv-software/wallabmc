/*
 * SPDX-FileCopyrightText: © 2026 Red Hat, LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>

#include "bootsel.h"

LOG_MODULE_REGISTER(bootsel, LOG_LEVEL_INF);

#define BOOTSEL_COUNT 4

static const struct gpio_dt_spec bootsel_gpios[BOOTSEL_COUNT] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(bootsel_0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(bootsel_1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(bootsel_2), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(bootsel_3), gpios),
};

/* false = HW mode (input, follows DIP switch), true = SW mode (output) */
static bool sw_mode;

int bootsel_set_hw_mode(void)
{
	for (int i = 0; i < BOOTSEL_COUNT; i++) {
		int ret = gpio_pin_configure_dt(&bootsel_gpios[i], GPIO_INPUT);
		if (ret < 0) {
			LOG_ERR("Failed to set BOOT_SEL%d to input: %d", i, ret);
			return ret;
		}
	}
	sw_mode = false;
	LOG_INF("Boot select: hardware mode (following DIP switch)");
	return 0;
}

int bootsel_set_sw_mode(uint8_t value)
{
	for (int i = 0; i < BOOTSEL_COUNT; i++) {
		int ret = gpio_pin_configure_dt(&bootsel_gpios[i],
						GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("Failed to set BOOT_SEL%d to output: %d",
				i, ret);
			return ret;
		}
		ret = gpio_pin_set_dt(&bootsel_gpios[i], (value >> i) & 1);
		if (ret < 0) {
			LOG_ERR("Failed to drive BOOT_SEL%d: %d", i, ret);
			return ret;
		}
	}
	sw_mode = true;
	LOG_INF("Boot select: software mode, value=0x%x", value);
	return 0;
}

int bootsel_read(uint8_t *value)
{
	*value = 0;
	for (int i = 0; i < BOOTSEL_COUNT; i++) {
		int val = gpio_pin_get_dt(&bootsel_gpios[i]);
		if (val < 0) {
			return val;
		}
		*value |= (val & 1) << i;
	}
	return 0;
}

bool bootsel_is_sw_mode(void)
{
	return sw_mode;
}

const char *bootsel_boot_source(uint8_t sel)
{
	switch (sel & 0x0F) {
	case 0x00: return "SCPU ROM -> UART";
	case 0x01: return "SCPU ROM -> eMMC";
	case 0x02: return "SCPU ROM -> SPI NOR";
	case 0x03: return "SCPU ROM -> USB";
	case 0x04: return "SCPU SPI NOR -> UART";
	case 0x05: return "SCPU SPI NOR -> eMMC";
	case 0x06: return "SCPU SPI NOR -> SPI NOR";
	case 0x07: return "SCPU SPI NOR -> USB";
	default:
		if ((sel & 0x03) == 0x00) return "U84 SPI NOR -> UART";
		if ((sel & 0x03) == 0x01) return "U84 SPI NOR -> eMMC";
		if ((sel & 0x03) == 0x02) return "U84 SPI NOR -> SPI NOR";
		if ((sel & 0x03) == 0x03) return "U84 SPI NOR -> USB";
		return "Unknown";
	}
}

static int cmd_bootsel_get(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t val;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = bootsel_read(&val);
	if (ret < 0) {
		shell_error(sh, "Failed to read boot select: %d", ret);
		return ret;
	}

	shell_print(sh, "Mode: %s", sw_mode ? "SOFTWARE" : "HARDWARE (DIP switch)");
	shell_print(sh, "BOOT_SEL[3:0] = 0x%x (0b%d%d%d%d)",
		    val,
		    (val >> 3) & 1, (val >> 2) & 1,
		    (val >> 1) & 1, (val >> 0) & 1);
	shell_print(sh, "Boot source: %s", bootsel_boot_source(val));

	return 0;
}

static int cmd_bootsel_set(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: bootsel set <0-15|hw>");
		return -EINVAL;
	}

	if (strcmp(argv[1], "hw") == 0) {
		return bootsel_set_hw_mode();
	}

	unsigned long val = strtoul(argv[1], NULL, 0);
	if (val > 15) {
		shell_error(sh, "Value must be 0-15 (4-bit boot select)");
		return -EINVAL;
	}

	int ret = bootsel_set_sw_mode((uint8_t)val);
	if (ret < 0) {
		shell_error(sh, "Failed to set boot select: %d", ret);
		return ret;
	}

	shell_print(sh, "Boot select set to 0x%lx (%s)", val,
		    bootsel_boot_source((uint8_t)val));

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_bootsel_cmds,
	SHELL_CMD(get, NULL, "Get current boot select configuration", cmd_bootsel_get),
	SHELL_CMD_ARG(set, NULL,
		"Set boot select: <0-15> for SW mode, 'hw' for HW (DIP switch) mode",
		cmd_bootsel_set, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(bootsel, &sub_bootsel_cmds, "SOM boot mode selection", NULL);

int bootsel_init(void)
{
	for (int i = 0; i < BOOTSEL_COUNT; i++) {
		if (!gpio_is_ready_dt(&bootsel_gpios[i])) {
			LOG_ERR("BOOT_SEL%d GPIO not ready", i);
			return -ENODEV;
		}
	}

	return bootsel_set_hw_mode();
}
