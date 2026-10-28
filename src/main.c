/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
LOG_MODULE_REGISTER(wallabmc, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/shell/shell.h>

#include "fs.h"
#include "config.h"
#include "button.h"
#include "net.h"
#include "wifi.h"
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

#define STATUS_LED DT_ALIAS(status_led)

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

#if DT_NODE_HAS_STATUS_OKAY(STATUS_LED)
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(STATUS_LED, gpios);

/* LED blinking thread */
#define LED_BLINK_PERIOD_DOT    250
#define LED_BLINK_PERIOD_DASH   (3 * LED_BLINK_PERIOD_DOT)
#define LED_BLINK_PERIOD_PAUSE  (LED_BLINK_PERIOD_DOT)
#define LED_BLINK_PERIOD_LETTER (3 * LED_BLINK_PERIOD_DOT)
#define LED_BLINK_PERIOD_WORD   (7 * LED_BLINK_PERIOD_DOT)
#define LED_BLINK_PERIOD_PANIC_US 50000

static void led_dash(void)
{
	gpio_pin_set_dt(&status_led, 1);
	k_msleep(LED_BLINK_PERIOD_DASH);
	gpio_pin_set_dt(&status_led, 0);
	k_msleep(LED_BLINK_PERIOD_PAUSE);
}

static void led_dot(void)
{
	gpio_pin_set_dt(&status_led, 1);
	k_msleep(LED_BLINK_PERIOD_DOT);
	gpio_pin_set_dt(&status_led, 0);
	k_msleep(LED_BLINK_PERIOD_PAUSE);
}

static void status_led_okay(void)
{
	// Flash OK in morse code: - - -   - . -
	while (1) {
		// O
		led_dash();
		led_dash();
		led_dash();

		k_msleep(LED_BLINK_PERIOD_LETTER);

		// K
		led_dash();
		led_dot();
		led_dash();

		// Pause
		k_msleep(LED_BLINK_PERIOD_WORD);
	}
}

static void status_led_fail(void)
{
	while (1)
		led_dot();
}

static void status_led_blink(bool fail)
{
	if (!gpio_is_ready_dt(&status_led))
		return;

	if (fail)
		status_led_fail();
	else
		status_led_okay();
}

static void status_led_panic(void)
{
	if (!gpio_is_ready_dt(&status_led))
		return;

	while (1) {
		gpio_pin_toggle_dt(&status_led);
		/* No sleeping in panic handler */
		k_busy_wait(LED_BLINK_PERIOD_PANIC_US);
	}
}

static int status_led_init(void)
{
	int ret;

	/* Initialize status LED */
	if (gpio_is_ready_dt(&status_led)) {
		ret = gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_INF("Could not configure LED1 GPIO\n");
			return -1;
		}
		gpio_pin_set_dt(&status_led, 1);
	} else {
		LOG_INF("LED1 GPIO not ready\n");
		return -1;
	}

	return 0;
}

#else /* DT_NODE_HAS_STATUS_OKAY(STATUS_LED) */
static void status_led_blink(bool fail)
{
}

static void status_led_panic(void)
{
}

static int status_led_init(void)
{
	return 0;
}
#endif /* DT_NODE_HAS_STATUS_OKAY(STATUS_LED) */

void k_sys_fatal_error_handler(unsigned int reason,
			       const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	LOG_PANIC();
	LOG_ERR("Halting system!");
	(void)irq_lock();
	status_led_panic();
	for (;;)
		;
	CODE_UNREACHABLE;
}

FUNC_NORETURN int bmc_reboot(void)
{
	net_exit();
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

#ifdef CONFIG_APP_WIFI
static int cmd_wifi_connect(const struct shell *sh, size_t argc, char **argv)
{
	const char *ssid = NULL;
	const char *password = NULL;
	int rc;

	if (argc >= 2)
		ssid = argv[1];
	if (argc >= 3)
		password = argv[2];

	if (!ssid)
		ssid = config_bmc_wifi_ssid();
	if (!password)
		password = config_bmc_wifi_password();

	rc = wifi_connect(ssid, password);
	if (rc) {
		shell_error(sh, "Wi-Fi connect failed (err=%d)", rc);
		return rc;
	}

	shell_info(sh, "Wi-Fi connecting to %s", ssid);

	return 0;
}

static int cmd_wifi_disconnect(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	wifi_disconnect();

	shell_info(sh, "Wi-Fi disconnect");

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_bmc_wifi_cmds,
	SHELL_CMD_ARG(connect,	NULL,
		      "Connect to a Wi-Fi network optionally overriding default credentials.\n"
		      "Usage: wifi connect [ssid] [password]",
		      cmd_wifi_connect, 1, 2),
	SHELL_CMD(disconnect,	NULL, "Disconnect from Wi-Fi AP.", cmd_wifi_disconnect),
	SHELL_SUBCMD_SET_END
);
#endif /* CONFIG_APP_WIFI */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_bmc_cmds,
	SHELL_CMD(reboot,	NULL, "Reboot BMC.", cmd_bmc_reboot),
	SHELL_CMD(poweroff,	NULL, "Power off BMC.", cmd_bmc_poweroff),
#ifdef CONFIG_APP_WIFI
	SHELL_CMD(wifi,		&sub_bmc_wifi_cmds, "Wi-Fi control commands.", NULL),
#endif
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
	/*
	 * If fail_status is set, the host will not be auto-powered on,
	 * and the status LED will blink fast. The system might still be
	 * usable or at least debuggable, so don't fail entirely.
	 *
	 * Expected or tolerable errors can avoid setting fail_status.
	 */
	bool fail_status = false;

	print_banner();

	/* Turn on the status LED early */
	LOG_DBG("LED init");
	if (status_led_init() < 0) {
		LOG_ERR("LED init failed");
		/* Continue */
	}

	LOG_DBG("RTC init");
	if (rtc_init() < 0) {
		LOG_ERR("RTC init failed");
		fail_status = true;
	}

	LOG_DBG("VPD init");
	if (vpd_init() < 0) {
		LOG_ERR("VPD init failed");
		fail_status = true;
	}

	LOG_DBG("Filesystem init");
	if (fs_init() < 0) {
		LOG_ERR("Filesystem init failed, continuing without persistent storage");
	}

	LOG_DBG("Config init");
	if (config_init() < 0) {
		LOG_ERR("Config init failed");
		fail_status = true;
	}

	/*
	 * After the basics are up
	 */

	LOG_DBG("Button init");
	if (button_init() < 0) {
		LOG_ERR("Button init failed");
		fail_status = true;
	}

	LOG_DBG("Network init");
	if (net_init() < 0) {
		LOG_ERR("Network init failed");
		/* Continue -- shell stays up so the user can `wifi connect`, etc. */
	}

	LOG_DBG("Power init");
	if (power_init() < 0) {
		LOG_ERR("Power init failed");
		fail_status = true;
	}

	LOG_DBG("Reset init");
	if (reset_init() < 0) {
		LOG_ERR("Reset init failed");
		fail_status = true;
	}

	LOG_DBG("JTAG init");
	if (jtag_init() < 0) {
		LOG_ERR("JTAG init failed");
		fail_status = true;
	}

	LOG_DBG("Console logger init");
	if (console_logger_init() < 0) {
		LOG_ERR("Console logger init failed");
		fail_status = true;
	}

	/*
	 * Wait until BMC host interfaces such as power, reset, status are
	 * initialised, host UART logger is running, etc., before host
	 * auto-poweron.
	 *
	 * If auto-poweron fails, a message will be logged. Don't
	 * fail BMC boot, because the admin may want to diagnose
	 * and try power cycling etc.
	 *
	 * If something has failed, don't try to auto-poweron.
	 */
	if (fail_status)
		LOG_WRN("Boot failures encountered, not powering on host");
	else
		power_auto_poweron();

	/*
	 * Interfaces to outside world are brought up after auto poweron, so
	 * they won't race with one another. The zephyr console interface is
	 * generally gated by the boot_finished flag so it should not race with
	 * auto poweron either.
	 */
	LOG_DBG("HTTP server init");
	if (app_http_server_init() < 0) {
		LOG_ERR("HTTP server init failed");
		fail_status = true;
	}

	LOG_DBG("Console bridge init");
	if (console_bridge_init() < 0) {
		LOG_ERR("Console bridge init failed");
		fail_status = true;
	}

	LOG_DBG("Console bridge WS init");
	if (console_bridge_ws_init() < 0) {
		LOG_ERR("Console bridge WS init failed");
		fail_status = true;
	}

	boot_finished = true;

	LOG_INF("BMC boot complete");

	status_led_blink(fail_status);

	/* If no LED, this will fall through and exit the main thread */

	return 0;
}
