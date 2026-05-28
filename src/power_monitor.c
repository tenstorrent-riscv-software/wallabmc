/*
 * SPDX-FileCopyrightText: © 2026 Red Hat, LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>

#include "power.h"

LOG_MODULE_REGISTER(power_monitor, LOG_LEVEL_INF);

#define INA226_NODE DT_NODELABEL(ina226)

#if DT_NODE_EXISTS(INA226_NODE)
static const struct device *ina226_dev = DEVICE_DT_GET(INA226_NODE);

int power_monitor_read(int32_t *voltage_mv, int32_t *current_ma,
		       int32_t *power_mw)
{
	struct sensor_value val;
	int ret;

	if (!power_get_state()) {
		return -ENODEV;
	}

	if (!device_is_ready(ina226_dev)) {
		ina226_dev->state->initialized = false;
		ret = device_init(ina226_dev);
		if (ret < 0) {
			return ret;
		}
	}

	ret = sensor_sample_fetch(ina226_dev);
	if (ret < 0) {
		LOG_ERR("INA226 sample fetch failed: %d", ret);
		return ret;
	}

	ret = sensor_channel_get(ina226_dev, SENSOR_CHAN_VOLTAGE, &val);
	if (ret == 0 && voltage_mv) {
		*voltage_mv = val.val1 * 1000 + val.val2 / 1000;
	}

	ret = sensor_channel_get(ina226_dev, SENSOR_CHAN_CURRENT, &val);
	if (ret == 0 && current_ma) {
		*current_ma = val.val1 * 1000 + val.val2 / 1000;
	}

	ret = sensor_channel_get(ina226_dev, SENSOR_CHAN_POWER, &val);
	if (ret == 0 && power_mw) {
		*power_mw = val.val1 * 1000 + val.val2 / 1000;
	}

	return 0;
}

static int cmd_power_info(const struct shell *sh, size_t argc, char **argv)
{
	int32_t voltage_mv, current_ma, power_mw;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = power_monitor_read(&voltage_mv, &current_ma, &power_mw);
	if (ret == -ENODEV) {
		shell_error(sh, "Power monitor unavailable (host power off)");
		return ret;
	}
	if (ret < 0) {
		shell_error(sh, "Failed to read power monitor: %d", ret);
		return ret;
	}

	shell_print(sh, "12V Rail:");
	shell_print(sh, "  Voltage: %d.%03d V", voltage_mv / 1000,
		    voltage_mv % 1000);
	shell_print(sh, "  Current: %d.%03d A", current_ma / 1000,
		    current_ma % 1000);
	shell_print(sh, "  Power:   %d.%03d W", power_mw / 1000,
		    power_mw % 1000);

	return 0;
}

SHELL_CMD_REGISTER(power_info, NULL, "Show 12V rail power (INA226)",
		   cmd_power_info);

#endif /* DT_NODE_EXISTS(INA226_NODE) */
