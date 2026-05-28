/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
LOG_MODULE_REGISTER(wallabmc, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/shell/shell.h>

#include "fs.h"
#include "config.h"
#include "button.h"
#include "net.h"
#include "http.h"
#include "power.h"
#include "rtc.h"
#include "sensors.h"
#include "jtag.h"
#include "console_logger.h"
#include "console_bridge.h"
#include "console_bridge_ws.h"
#include "vpd.h"
#include "git_sha.h"
#include "fan.h"

static bool boot_finished = false;

bool is_boot_finished(void)
{
	return boot_finished;
}

static void print_banner(void)
{
	int pad = 20;

	printk("\n");
	printk("%*s    +---------------------+\n", pad, "");
	printk("%*s    | Welcome to WallaBMC |\n", pad, "");
	printk("%*s    +---------------------+\n", pad, "");
	printk("%*s                 \\\n", pad, "");
	printk("%*s                   \\     /)\n", pad, "");
	printk("%*s                      <.' \\_\n", pad, "");
	printk("%*s                        / ( )\\\n", pad, "");
	printk("%*s                        __|/  \\__\n", pad, "");
	printk("%*s.,.,.,,,..,,.,.,.,.,..,,.,.,.,,.,..,,.,.,.,.\n", pad, "");

	LOG_INF("Project build: %s", PROJECT_GIT_SHA);
	LOG_INF("Zephyr OS build: %s", BANNER_VERSION);
#if defined(CONFIG_REDFISH)
	LOG_INF("Board: %s", CONFIG_REDFISH_SYSTEM_PRODUCT_NAME);
	LOG_INF("System: %s %s (CPU: %s x%d, Memory: %d GiB)",
		CONFIG_REDFISH_SYSTEM_MANUFACTURER,
		CONFIG_REDFISH_SYSTEM_MODEL,
		CONFIG_REDFISH_SYSTEM_PROCESSOR_MODEL,
		CONFIG_REDFISH_SYSTEM_PROCESSOR_COUNT,
		CONFIG_REDFISH_SYSTEM_MEMORY_GIB);
#endif
}

FUNC_NORETURN int bmc_reboot(void)
{
	fs_exit();

	LOG_WRN("Rebooting BMC");
	/* log_panic() appears to be needed in order to get the logs out. */
	log_panic();
	k_msleep(100);

	/*
	 * It is said that not all platforms support all reboot types.
	 * Not sure the best way to do this, but try a warm reboot first,
	 * then cold.
	 */
	sys_reboot(SYS_REBOOT_WARM);
	sys_reboot(SYS_REBOOT_COLD);
	k_panic();
	for (;;);
}

static FUNC_NORETURN int bmc_poweroff(void)
{
	fs_exit();

	LOG_WRN("Poweroff BMC");
	/* log_panic() appears to be needed in order to get the logs out. */
	log_panic();
	k_msleep(100);

#ifdef CONFIG_POWEROFF
	sys_poweroff();
	k_panic();
#endif
	for (;;);
}

static int cmd_bmc_reboot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	bmc_reboot();
	return 0;
}

static int cmd_bmc_poweroff(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	bmc_poweroff();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_bmc_cmds,
	SHELL_CMD(reboot,	NULL, "Reboot BMC.", cmd_bmc_reboot),
	SHELL_CMD(poweroff,	NULL, "Power off BMC.", cmd_bmc_poweroff),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(bmc, &sub_bmc_cmds, "BMC system commands", NULL);

static int cmd_hop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	/* Wallaby on ground */
	const char *wallaby_ground[] = {
		"",
		"",
		"",
		"             /)",
		"          <.' \\_",
		"            / ( )\\",
		"            __|/  \\__",
		NULL
	};

	/* Wallaby in air (hopped up) */
	const char *wallaby_air[] = {
		"             /)",
		"          <.' \\_",
		"            / ( )\\",
		"            |/  \\",
		"            /     \\",
		"",
		"",
		NULL
	};

	int pos;  /* Horizontal position */
	int i, j;
	int max_pos = 60;  /* Maximum horizontal position */
	int num_hops = 4;  /* Number of complete hops */
	bool in_air = false;

	shell_print(sh, "\n");

	for (i = 0; i < num_hops; i++) {
		/* Each hop moves the wallaby forward */
		for (pos = max_pos; pos > 0; pos -= 4) {
			/* Clear screen and move cursor to home position */
			shell_print(sh, "\033[2J\033[H");

			/* Alternate between air and ground every few steps */
			if ((pos / 8) % 2 == 0) {
				in_air = false;
			} else {
				in_air = true;
			}

			const char **current_frame = in_air ? wallaby_air : wallaby_ground;

			/* Print the frame with horizontal offset */
			for (j = 0; current_frame[j] != NULL; j++) {
					shell_print(sh, "%*s%s", pos, "", current_frame[j]);
			}
			shell_print(sh, "----------------------------------------------------------------------------------");

			k_msleep(120);  /* Animation speed */
		}
	}

	shell_print(sh, "\n");

	return 0;
}

SHELL_CMD_REGISTER(hop, NULL, "Make the wallaby hop across the screen.", cmd_hop);

int main(void)
{
	print_banner();

	LOG_DBG("RTC init");
	if (rtc_init() < 0) {
		LOG_ERR("RTC init failed");
		return -1;
	}

	LOG_DBG("VPD init");
	if (vpd_init() < 0) {
		LOG_ERR("VPD init failed");
		return -1;
	}

	LOG_DBG("Filesystem init");
	if (fs_init() < 0) {
		LOG_ERR("Filesystem init failed, continuing without persistent storage");
	}

	LOG_DBG("Config init");
	if (config_init() < 0) {
		LOG_ERR("Config init failed");
		return -1;
	}

	LOG_DBG("Button init");
	if (button_init() < 0) {
		LOG_ERR("Button init failed");
		/* Continue */
	}

	LOG_DBG("Network init");
	if (net_init() < 0) {
		LOG_ERR("Network init failed");
		return -1;
	}

	LOG_DBG("Power init");
	if (power_init() < 0) {
		LOG_ERR("Power init failed");
		return -1;
	}

	LOG_DBG("Fan init");
	if (fan_init() < 0) {
		LOG_ERR("Fan init failed");
		/* Continue */
	}

	LOG_DBG("Reset init");
	if (reset_init() < 0) {
		LOG_ERR("Reset init failed");
		return -1;
	}

	LOG_DBG("LED init");
	if (status_led_init() < 0) {
		LOG_ERR("LED init failed");
		return -1;
	}

	LOG_DBG("JTAG init");
	if (jtag_init() < 0) {
		LOG_ERR("JTAG init failed");
		return -1;
	}

	LOG_DBG("HTTP server init");
	if (app_http_server_init() < 0) {
		LOG_ERR("HTTP server init failed");
		return -1;
	}

	LOG_DBG("Console logger init");
	if (console_logger_init() < 0) {
		LOG_ERR("Console logger init failed");
		return -1;
	}

	LOG_DBG("Console bridge init");
	if (console_bridge_init() < 0) {
		LOG_ERR("Console bridge init failed");
		return -1;
	}

	LOG_DBG("Console bridge WS init");
	if (console_bridge_ws_init() < 0) {
		LOG_ERR("Console bridge WS init failed");
		return -1;
	}

	boot_finished = true;

	LOG_INF("BMC boot complete");

	return 0;
}
