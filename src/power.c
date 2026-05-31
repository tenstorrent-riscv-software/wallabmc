/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <ctype.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wallabmc_power);

#include "config.h"

/* Forward declarations */
int power_reset(void);
#ifdef CONFIG_SOM_PROTOCOL
static void power_graceful_init(void);
int power_graceful_off(void);
#endif

#define GPIO_POWER_1 DT_ALIAS(power_gpio_1)
#define GPIO_POWER_2 DT_ALIAS(power_gpio_2)
#define GPIO_RESET DT_ALIAS(reset_gpio_1)
#define STATUS_LED DT_ALIAS(status_led)
#define GPIO_POWER_GOOD DT_ALIAS(power_good)
#define GPIO_POWER_LED  DT_ALIAS(power_led)
#define GPIO_SLEEP_LED  DT_ALIAS(sleep_led)
#define GPIO_POWER_BTN  DT_ALIAS(power_button)

static const struct gpio_dt_spec power_gpios[] = {
#if DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_1)
	GPIO_DT_SPEC_GET(GPIO_POWER_1, gpios),
#endif
#if DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_2)
	GPIO_DT_SPEC_GET(GPIO_POWER_2, gpios),
#endif
};

#if DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_GOOD)
static const struct gpio_dt_spec power_good_gpio =
	GPIO_DT_SPEC_GET(GPIO_POWER_GOOD, gpios);
#endif

#if DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_LED)
static const struct gpio_dt_spec power_led_gpio =
	GPIO_DT_SPEC_GET(GPIO_POWER_LED, gpios);
#endif

#if DT_NODE_HAS_STATUS_OKAY(GPIO_SLEEP_LED)
static const struct gpio_dt_spec sleep_led_gpio =
	GPIO_DT_SPEC_GET(GPIO_SLEEP_LED, gpios);
#endif

static const struct gpio_dt_spec gpio_reset =
#if DT_NODE_HAS_STATUS_OKAY(GPIO_RESET)
	GPIO_DT_SPEC_GET(GPIO_RESET, gpios);
#else
	{ 0 };
#endif

static bool system_power_state = false;

bool power_get_state(void)
{
	return system_power_state;
}

static int power_on(void)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(power_gpios); i++) {
		ret = gpio_pin_set_dt(&power_gpios[i], 1);
		if (ret < 0) {
			LOG_ERR("Could not assert power GPIO %d", i);
			return -1;
		}
	}

#if DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_GOOD)
	int retries = CONFIG_POWER_GOOD_TIMEOUT_MS / CONFIG_POWER_GOOD_POLL_MS;

	while (retries > 0) {
		k_msleep(CONFIG_POWER_GOOD_POLL_MS);
		if (gpio_pin_get_dt(&power_good_gpio) == 1) {
			break;
		}
		retries--;
	}

	if (retries <= 0) {
		LOG_ERR("Power-good timeout, aborting power on");
		for (i = 0; i < ARRAY_SIZE(power_gpios); i++) {
			gpio_pin_set_dt(&power_gpios[i], 0);
		}
		return -ETIMEDOUT;
	}

	LOG_INF("Power good detected");
#endif

#if DT_NODE_HAS_STATUS_OKAY(GPIO_RESET)
	gpio_pin_set_dt(&gpio_reset, 0);
	LOG_INF("SOM reset released");
#endif

#if DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_LED)
	gpio_pin_set_dt(&power_led_gpio, 1);
#endif
#if DT_NODE_HAS_STATUS_OKAY(GPIO_SLEEP_LED)
	gpio_pin_set_dt(&sleep_led_gpio, 0);
#endif

	system_power_state = true;

	return 0;
}

static int power_off(void)
{
	int i, ret;

#if DT_NODE_HAS_STATUS_OKAY(GPIO_RESET)
	gpio_pin_set_dt(&gpio_reset, 1);
	k_msleep(10);
#endif

#if DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_LED)
	gpio_pin_set_dt(&power_led_gpio, 0);
#endif
#if DT_NODE_HAS_STATUS_OKAY(GPIO_SLEEP_LED)
	gpio_pin_set_dt(&sleep_led_gpio, 1);
#endif

	for (i = 0; i < ARRAY_SIZE(power_gpios); i++) {
		ret = gpio_pin_set_dt(&power_gpios[i], 0);
		if (ret < 0) {
			LOG_ERR("Could not deassert power GPIO %d", i);
			return -1;
		}
	}

	system_power_state = false;

	return 0;
}

int power_set_state(bool on)
{
	int ret;

	if (on)
		ret = power_on();
	else
		ret = power_off();

	if (ret < 0) {
		LOG_ERR("Failed to set power state: %d", ret);
		return ret;
	}

	LOG_INF("System Power State changed to: %s", power_get_state() ? "ON" : "OFF");
	return 0;
}

#if DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_BTN)
static const struct gpio_dt_spec power_btn_gpio =
	GPIO_DT_SPEC_GET(GPIO_POWER_BTN, gpios);
static struct gpio_callback power_btn_cb_data;

static void power_btn_work_fn(struct k_work *work)
{
	if (power_get_state()) {
#ifdef CONFIG_SOM_PROTOCOL
		LOG_INF("Power button: graceful shutdown");
		power_graceful_off();
#else
		LOG_INF("Power button: power off");
		power_set_state(false);
#endif
	} else {
		LOG_INF("Power button: power on");
		power_set_state(true);
	}
}

static K_WORK_DEFINE(power_btn_work, power_btn_work_fn);

static void power_btn_isr(const struct device *dev,
			   struct gpio_callback *cb, uint32_t pins)
{
	k_work_submit(&power_btn_work);
}
#endif

int power_init(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(power_gpios); i++) {
		if (!gpio_is_ready_dt(&power_gpios[i])) {
			LOG_INF("Power GPIO %d not ready\n", i);
			return -1;
		}

		if (gpio_pin_configure_dt(&power_gpios[i], GPIO_OUTPUT_INACTIVE) < 0) {
			LOG_INF("Could not configure power GPIO %d\n", i);
			return -1;
		}
	}

#if DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_GOOD)
	if (!gpio_is_ready_dt(&power_good_gpio)) {
		LOG_ERR("Power-good GPIO not ready");
		return -1;
	}
	if (gpio_pin_configure_dt(&power_good_gpio, GPIO_INPUT) < 0) {
		LOG_ERR("Could not configure power-good GPIO");
		return -1;
	}
#endif

#if DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_LED)
	if (gpio_is_ready_dt(&power_led_gpio)) {
		gpio_pin_configure_dt(&power_led_gpio, GPIO_OUTPUT_INACTIVE);
	}
#endif
#if DT_NODE_HAS_STATUS_OKAY(GPIO_SLEEP_LED)
	if (gpio_is_ready_dt(&sleep_led_gpio)) {
		gpio_pin_configure_dt(&sleep_led_gpio, GPIO_OUTPUT_ACTIVE);
	}
#endif

#if DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_BTN)
	if (gpio_is_ready_dt(&power_btn_gpio)) {
		gpio_pin_configure_dt(&power_btn_gpio, GPIO_INPUT);
		gpio_pin_interrupt_configure_dt(&power_btn_gpio,
						GPIO_INT_EDGE_TO_ACTIVE);
		gpio_init_callback(&power_btn_cb_data, power_btn_isr,
				   BIT(power_btn_gpio.pin));
		gpio_add_callback(power_btn_gpio.port, &power_btn_cb_data);
		LOG_INF("Power button initialized");
	}
#endif

	if (config_host_auto_poweron()) {
		int ret = power_on();
		if (ret < 0) {
			LOG_ERR("Auto power-on failed: %d", ret);
			return ret;
		}
	}

#ifdef CONFIG_SOM_PROTOCOL
	power_graceful_init();
#endif

	return 0;
}

static int cmd_power_on(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return power_on();
}

static int cmd_power_off(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return power_off();
}

#ifdef CONFIG_SOM_PROTOCOL
static struct k_timer shutdown_timer;
static struct k_timer reboot_timer;

static void shutdown_timer_expiry(struct k_timer *timer)
{
	LOG_WRN("SOM shutdown timeout, forcing power off");
	power_off();
}

static void reboot_timer_expiry(struct k_timer *timer)
{
	LOG_WRN("SOM reboot timeout, forcing reset");
	power_reset();
}

static void som_power_notify(uint8_t cmd_type)
{
	if (cmd_type == SOM_CMD_POWER_OFF) {
		k_timer_stop(&shutdown_timer);
		LOG_INF("SOM acknowledged shutdown, powering off");
		power_off();
	} else if (cmd_type == SOM_CMD_RESTART) {
		k_timer_stop(&reboot_timer);
		LOG_INF("SOM acknowledged restart, resetting");
		power_reset();
	}
}

int power_graceful_off(void)
{
	int ret;

	if (!power_get_state()) {
		return 0;
	}

	ret = som_cmd(SOM_CMD_POWER_OFF, NULL, 0,
		      CONFIG_SOM_PROTOCOL_TX_TIMEOUT_MS);
	if (ret < 0) {
		LOG_WRN("Could not send shutdown to SOM, forcing off");
		return power_off();
	}

	k_timer_start(&shutdown_timer,
		       K_MSEC(CONFIG_GRACEFUL_SHUTDOWN_TIMEOUT_MS), K_NO_WAIT);

	LOG_INF("Graceful shutdown initiated, timeout %d ms",
		CONFIG_GRACEFUL_SHUTDOWN_TIMEOUT_MS);
	return 0;
}

int power_graceful_restart(void)
{
	int ret;

	if (!power_get_state()) {
		return power_on();
	}

	ret = som_cmd(SOM_CMD_RESTART, NULL, 0,
		      CONFIG_SOM_PROTOCOL_TX_TIMEOUT_MS);
	if (ret < 0) {
		LOG_WRN("Could not send restart to SOM, forcing reset");
		return power_reset();
	}

	k_timer_start(&reboot_timer,
		       K_MSEC(CONFIG_GRACEFUL_REBOOT_TIMEOUT_MS), K_NO_WAIT);

	LOG_INF("Graceful restart initiated, timeout %d ms",
		CONFIG_GRACEFUL_REBOOT_TIMEOUT_MS);
	return 0;
}

static void power_graceful_init(void)
{
	k_timer_init(&shutdown_timer, shutdown_timer_expiry, NULL);
	k_timer_init(&reboot_timer, reboot_timer_expiry, NULL);
	som_set_notify_callback(som_power_notify);
}

static int cmd_power_shutdown(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return power_graceful_off();
}

static int cmd_power_restart(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return power_graceful_restart();
}
#endif /* CONFIG_SOM_PROTOCOL */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_power_cmds,
	SHELL_CMD(on,    NULL, "Power on.", cmd_power_on),
	SHELL_CMD(off,   NULL, "Force power off.", cmd_power_off),
#ifdef CONFIG_SOM_PROTOCOL
	SHELL_CMD(shutdown, NULL, "Graceful shutdown (ask SOM first).", cmd_power_shutdown),
	SHELL_CMD(restart, NULL, "Graceful restart (ask SOM first).", cmd_power_restart),
#endif
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(power, &sub_power_cmds, "Power commands", NULL);

int reset_init(void)
{
	int ret;

	if (!DT_NODE_HAS_STATUS_OKAY(GPIO_RESET)) {
		/* No reset GPIO */
		return 0;
	}

	if (!gpio_is_ready_dt(&gpio_reset)) {
		LOG_INF("Reset GPIO not ready\n");
		return -1;
	}

	ret = gpio_pin_configure_dt(&gpio_reset, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_INF("Could not configure reset GPIO\n");
		return -1;
	}

	ret = gpio_pin_set_dt(&gpio_reset, 0);
	if (ret < 0) {
		LOG_INF("Could not toggle reset GPIO\n");
		return -1;
	}

	return 0;
}

int power_reset(void)
{
	int ret;

	if (!DT_NODE_HAS_STATUS_OKAY(GPIO_RESET)) {
		LOG_ERR("No reset GPIO");
		return -1;
	}
	ret = gpio_pin_set_dt(&gpio_reset, 1);
	if (ret < 0) {
		LOG_INF("Could not toggle RESET GPIO\n");
		return -1;
	}

	k_msleep(1000);

	ret = gpio_pin_set_dt(&gpio_reset, 0);
	if (ret < 0) {
		LOG_INF("Could not toggle RESET GPIO\n");
		return -1;
	}

	return 0;
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return power_reset();
}

SHELL_CMD_REGISTER(reset, NULL, "Reset.", cmd_reset);

#if DT_NODE_HAS_STATUS_OKAY(STATUS_LED)
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(STATUS_LED, gpios);

/* LED blinking thread */
#define LED_BLINK_STACK_SIZE 256
#define LED_BLINK_PRIORITY   (CONFIG_NUM_PREEMPT_PRIORITIES - 1)
#define LED_BLINK_PERIOD_DOT    250
#define LED_BLINK_PERIOD_DASH   (3 * LED_BLINK_PERIOD_DOT)
#define LED_BLINK_PERIOD_PAUSE  (LED_BLINK_PERIOD_DOT)
#define LED_BLINK_PERIOD_LETTER (3 * LED_BLINK_PERIOD_DOT)
#define LED_BLINK_PERIOD_WORD   (7 * LED_BLINK_PERIOD_DOT)

static K_KERNEL_STACK_DEFINE(led_blink_stack, LED_BLINK_STACK_SIZE);
static struct k_thread led_blink_thread_data;

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

static void led_blink_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!gpio_is_ready_dt(&status_led)) {
		return;
	}

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

int status_led_init(void)
{
	int ret;

	/* Initialize status LED */
	if (gpio_is_ready_dt(&status_led)) {
		ret = gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_INF("Could not configure LED1 GPIO\n");
			return -1;
		}
	} else {
		LOG_INF("LED1 GPIO not ready\n");
		return -1;
	}

	k_thread_create(&led_blink_thread_data, led_blink_stack,
			K_THREAD_STACK_SIZEOF(led_blink_stack),
			led_blink_thread,
			NULL, NULL, NULL,
			LED_BLINK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&led_blink_thread_data, "led_blink");

	return 0;
}
#else /* DT_NODE_HAS_STATUS_OKAY(STATUS_LED) */
int status_led_init(void)
{
	return 0;
}
#endif /* DT_NODE_HAS_STATUS_OKAY(STATUS_LED) */
