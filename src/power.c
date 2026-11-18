/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wallabmc_power);

#include "config.h"
#include "main.h"
#include "power.h"

#define GPIO_POWER_1 DT_ALIAS(power_gpio_1)
#define GPIO_POWER_2 DT_ALIAS(power_gpio_2)
#define GPIO_POWER_GOOD DT_ALIAS(host_power_good)
#define GPIO_RESET DT_ALIAS(reset_gpio_1)
#define GPIO_POWER_LED  DT_ALIAS(power_led)

static const struct gpio_dt_spec power_gpios[] = {
#if DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_1)
	GPIO_DT_SPEC_GET(GPIO_POWER_1, gpios),
#endif
#if DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_2)
	GPIO_DT_SPEC_GET(GPIO_POWER_2, gpios),
#endif
};

static const struct gpio_dt_spec power_led_gpio =
#if DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_LED)
	GPIO_DT_SPEC_GET(GPIO_POWER_LED, gpios);
#else
	{ 0 };
#endif

static const struct gpio_dt_spec gpio_reset =
#if DT_NODE_HAS_STATUS_OKAY(GPIO_RESET)
	GPIO_DT_SPEC_GET(GPIO_RESET, gpios);
#else
	{ 0 };
#endif

/*
 * Optional host power-good (PWROK) input, read only with a momentary
 * power button: the button gives no feedback, so without this input
 * the BMC cannot tell what the host is doing and must never press the
 * button on its own. A level-driven board drives the host power level
 * itself and never reads the input.
 */
#if defined(CONFIG_APP_HOST_POWER_PULSE) && DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_GOOD)
#define HAVE_POWER_GOOD 1
static const struct gpio_dt_spec power_good_gpio = GPIO_DT_SPEC_GET(GPIO_POWER_GOOD, gpios);
#else
#define HAVE_POWER_GOOD 0
static const struct gpio_dt_spec power_good_gpio = { 0 };
#endif

/*
 * One host power operation at a time. The power button is shared by
 * the shell, Redfish and auto power-on, on different threads, and a
 * momentary-button press is an assert / sleep / release sequence on
 * the caller's thread: a second caller releasing the pin after 200 ms
 * cuts a 6 s force-off short while both report success. A late caller
 * is told -EBUSY rather than queued, because a press that lands
 * seconds later arrives at a host whose state the first operation has
 * already changed; the operator is better told to look and try again.
 * The thread that asked sleeps through the hold itself rather than
 * queueing it: an item on the system workqueue must not sleep, and the
 * blocking workqueue runs one item at a time at low priority, so a
 * press queued there could sit behind an NTP attempt for seconds.
 * Callers are threads (shell, HTTP server, main) and must stay so; a
 * mutex cannot be taken from an ISR.
 *
 * Taken by the public entry points. The static power_on(),
 * power_off(), force_off() and power_pulse() workers assume it is
 * held. host_power_state() is deliberately lock-free.
 */
static K_MUTEX_DEFINE(host_power_mutex);

static int host_power_lock(void)
{
	int ret = k_mutex_lock(&host_power_mutex, K_NO_WAIT);

	if (ret < 0)
		LOG_WRN("Host power operation already in progress");

	return ret;	/* -EBUSY */
}

/*
 * Level-driven boards only: the level last written to the power GPIOs,
 * inactive at every BMC boot. A momentary button tells the BMC nothing,
 * so pulse mode never writes it and host_power_state() answers before
 * reaching it.
 */
static bool system_power_state = false;

/* 1 if the power-good input reads on, 0 if off, -errno if the read failed */
static int power_good_read(void)
{
	int ret = gpio_pin_get_dt(&power_good_gpio);

	if (ret < 0)
		LOG_ERR("Could not read power-good GPIO: %d", ret);

	return ret;
}

/*
 * See power.h. A level-driven board drives the host power level
 * itself, so the level it last wrote is the answer. A momentary
 * button gives no feedback: a press tells the BMC nothing about what
 * the host did with it, and the host can shut itself down or be
 * switched at the front panel, so without the input the state is
 * unknown, and a failed read is reported as unknown, not guessed.
 *
 * Lock-free: one GPIO read or one word, so Redfish GETs never wait
 * behind a press.
 */
int host_power_state(void)
{
	if (HAVE_POWER_GOOD)
		return power_good_read();

	if (IS_ENABLED(CONFIG_APP_HOST_POWER_PULSE))
		return -ENODATA;

	return system_power_state;
}

static const char *power_state_name(int state)
{
	if (state < 0)
		return "unknown";

	return state ? "on" : "off";
}

#ifdef CONFIG_APP_HOST_POWER_PULSE
/* Drive every power GPIO to val; try them all so one failure cannot leave another asserted */
static int power_button_set(int val)
{
	int i, ret = 0;

	for (i = 0; i < ARRAY_SIZE(power_gpios); i++) {
		if (gpio_pin_set_dt(&power_gpios[i], val) < 0) {
			LOG_ERR("Could not %s power GPIO %d",
				val ? "assert" : "release", i);
			ret = -1;
		}
	}

	return ret;
}

/* Hold the power button for ms on the caller's thread. host_power_mutex held. */
static int power_pulse(int ms)
{
	int ret, err;

	ret = power_button_set(1);
	if (ret == 0)
		k_msleep(ms);

	/*
	 * Release even after a failed assert: a GPIO that did assert would
	 * otherwise stay pressed, and a held button is a force-off.
	 */
	err = power_button_set(0);
	if (err < 0)
		LOG_ERR("Power button may still be held");

	return ret ? ret : err;
}

static int power_on(void)
{
	int state = host_power_state();

	if (state == 1) {
		LOG_INF("Host already on; skipping press");
		return 0;
	}

	/* A short press toggles: with the state unknown this may be a shutdown request */
	if (state < 0)
		LOG_WRN("Host power state unknown; pressing anyway "
			"(a running host is asked to shut down)");

	return power_pulse(CONFIG_APP_HOST_POWER_PULSE_MS);
}

static int power_off(void)
{
	int state = host_power_state();

	if (state == 0) {
		LOG_INF("Host already off; skipping press");
		return 0;
	}

	/* A short press toggles: with the state unknown this may turn an off host on */
	if (state < 0)
		LOG_WRN("Host power state unknown; pressing anyway "
			"(an off host is turned on)");

	return power_pulse(CONFIG_APP_HOST_POWER_PULSE_MS);
}

static int force_off(void)
{
	/*
	 * Skip only on a confirmed "off": force-off is the operator's last
	 * resort, so an unknown state must not disarm it.
	 */
	if (host_power_state() == 0) {
		LOG_INF("Host already off; skipping long press");
		return 0;
	}

	LOG_WRN("Forcing host off (long press)");
	return power_pulse(CONFIG_APP_HOST_POWER_FORCE_OFF_MS);
}

/* The short press itself, for a caller that has already decided it is right */
static int power_press(void)
{
	return power_pulse(CONFIG_APP_HOST_POWER_PULSE_MS);
}
#else /* !CONFIG_APP_HOST_POWER_PULSE */
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

	if (DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_LED) &&
	    gpio_is_ready_dt(&power_led_gpio)) {
		/* Don't fail the operation on LED failure */
		if (gpio_pin_set_dt(&power_led_gpio, 1) < 0)
			LOG_ERR("Could not assert power LED");
	}

	system_power_state = true;

	return 0;
}

static int power_off(void)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(power_gpios); i++) {
		ret = gpio_pin_set_dt(&power_gpios[i], 0);
		if (ret < 0) {
			LOG_ERR("Could not deassert power GPIO %d", i);
			return -1;
		}
	}

	if (DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_LED) &&
	    gpio_is_ready_dt(&power_led_gpio)) {
		/* Don't fail the operation on LED failure */
		if (gpio_pin_set_dt(&power_led_gpio, 0) < 0)
			LOG_ERR("Could not deassert power LED");
	}

	system_power_state = false;

	return 0;
}

static int force_off(void)
{
	return power_off();
}

static int power_press(void)
{
	return power_on();
}
#endif /* CONFIG_APP_HOST_POWER_PULSE */

int power_force_off(void)
{
	int ret;

	ret = host_power_lock();
	if (ret < 0)
		return ret;

	ret = force_off();

	k_mutex_unlock(&host_power_mutex);

	return ret;
}

int power_cycle(void)
{
	int ret;

	ret = host_power_lock();
	if (ret < 0)
		return ret;

	/*
	 * Force off unless the host is known to be off. On a
	 * momentary-button board the long press is the only thing that
	 * takes down a host whose state is unknown; a short press would
	 * only ask it to shut down.
	 */
	if (host_power_state() != 0) {
		ret = force_off();
		if (ret < 0)
			goto out;
	}

	k_msleep(2000);

	/*
	 * Only a power-good input can say this. Pressing now would be a
	 * shutdown request to a host that survived the force-off.
	 */
	if (host_power_state() == 1) {
		LOG_ERR("Host still on after force-off; not pressing power button");
		ret = -1;
		goto out;
	}

	/* Not through power_on(): its unknown-state warning is wrong after a force-off */
	ret = power_press();
out:
	k_mutex_unlock(&host_power_mutex);
	return ret;
}

int power_set_state(bool on)
{
	int ret, state;

	ret = host_power_lock();
	if (ret < 0)
		return ret;

	if (on)
		ret = power_on();
	else
		ret = power_off();

	k_mutex_unlock(&host_power_mutex);

	if (ret < 0) {
		LOG_ERR("Failed to set power state: %d", ret);
		return ret;
	}

	state = host_power_state();
	if (IS_ENABLED(CONFIG_APP_HOST_POWER_PULSE))
		/*
		 * A reading, not a change: a power-good input lags the press,
		 * and the press may have been skipped.
		 */
		LOG_INF("Host power state: %s", power_state_name(state));
	else
		LOG_INF("System Power State changed to: %s", state ? "ON" : "OFF");
	return 0;
}

int power_init(void)
{
	int i;

	/*
	 * There is no support for host power state to persist over BMC
	 * reboots yet. Power GPIOs are always initialised inactive and
	 * system_power_state is false at boot. On a momentary-button
	 * board that wires a host-power-good input, host_power_state()
	 * reads the real host state from it instead; a level-driven board
	 * drives the level itself and never reads the input.
	 */
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

	if (DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_LED) &&
	    gpio_is_ready_dt(&power_led_gpio)) {
		if (gpio_pin_configure_dt(&power_led_gpio, GPIO_OUTPUT_INACTIVE) < 0)
			LOG_INF("Could not configure power LED\n");
	}

	if (HAVE_POWER_GOOD) {
		int ret;

		if (!gpio_is_ready_dt(&power_good_gpio)) {
			LOG_INF("Power-good GPIO not ready\n");
			return -1;
		}

		if (gpio_pin_configure_dt(&power_good_gpio, GPIO_INPUT) < 0) {
			LOG_INF("Could not configure power-good GPIO\n");
			return -1;
		}

		ret = power_good_read();
		if (ret >= 0)
			LOG_INF("Host power-good input reads %s\n", ret ? "on" : "off");
	} else if (IS_ENABLED(CONFIG_APP_HOST_POWER_PULSE)) {
		LOG_WRN("No power-good input: host power state unknown; "
			"power on/off always press");
	}

	return 0;
}

int power_auto_poweron(void)
{
	int ret, state;

	if (!config_host_auto_poweron())
		return 0;

	ret = host_power_lock();
	if (ret < 0)
		return ret;

	state = host_power_state();
	if (state == 1) {
		LOG_INF("Host already on; skipping auto power-on");
		ret = 0;
		goto out;
	}

	/*
	 * Auto power-on is the one press the BMC sends on its own,
	 * unattended, so on a momentary button it needs a confirmed "off":
	 * a BMC-only restart (reboot, firmware update, power glitch) with
	 * the host running would otherwise send it a shutdown press. A
	 * level-driven board is never unknown: it drives the level.
	 */
	if (state < 0) {
		if (state == -ENODATA) {
			LOG_WRN("Host auto power-on ignored: no power-good input "
				"to tell whether the host is already on");
			ret = 0;
		} else {
			LOG_ERR("Not auto powering on: host power state unknown");
			ret = state;
		}
		goto out;
	}

	/* Decided on the read above: press directly rather than re-read in power_on() */
	ret = power_press();
	if (ret < 0)
		LOG_ERR("Auto power-on failed: %d", ret);
out:
	k_mutex_unlock(&host_power_mutex);
	return ret;
}

static int cmd_power_on(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!is_boot_finished()) {
		shell_error(sh, "must wait for boot to finish");
		return -EAGAIN;
	}

	rc = power_set_state(true);
	if (rc)
		shell_error(sh, "power on failed (err=%d)", rc);

	return rc;
}

static int cmd_power_off(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!is_boot_finished()) {
		shell_error(sh, "must wait for boot to finish");
		return -EAGAIN;
	}

	rc = power_set_state(false);
	if (rc)
		shell_error(sh, "power off failed (err=%d)", rc);

	return rc;
}

static int cmd_power_force_off(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!is_boot_finished()) {
		shell_error(sh, "must wait for boot to finish");
		return -EAGAIN;
	}

	rc = power_force_off();
	if (rc)
		shell_error(sh, "power force-off failed (err=%d)", rc);

	return rc;
}

static int cmd_power_state(const struct shell *sh, size_t argc, char **argv)
{
	int state;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!is_boot_finished()) {
		shell_error(sh, "must wait for boot to finish");
		return -EAGAIN;
	}

	state = host_power_state();
	if (state == -ENODATA)
		shell_print(sh, "Host power state: unknown (no power-good input)");
	else if (state < 0)
		shell_print(sh, "Host power state: unknown (power-good read failed, err=%d)",
			    state);
	else
		shell_print(sh, "Host power state: %s", power_state_name(state));

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_power_cmds,
	SHELL_CMD(on,        NULL, "Power on (short press).",  cmd_power_on),
	SHELL_CMD(off,       NULL, "Power off (short press).", cmd_power_off),
	SHELL_CMD(force-off, NULL, "Force off (long press).",  cmd_power_force_off),
	SHELL_CMD(state,     NULL, "Show host power state.",   cmd_power_state),
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

	/*
	 * Start inactive so the pin is hi-Z (open-drain) / driven inactive
	 * at boot. GPIO_OUTPUT_ACTIVE would briefly assert reset to the host
	 * every time the BMC starts up.
	 */
	ret = gpio_pin_configure_dt(&gpio_reset, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_INF("Could not configure reset GPIO\n");
		return -1;
	}

	return 0;
}

/*
 * If there is no reset GPIO, return -ENOTSUP so the caller can fall back
 * to power_cycle() to implement roughly equivalent behaviour.
 */
int power_reset(void)
{
	int ret, state;

	if (!DT_NODE_HAS_STATUS_OKAY(GPIO_RESET)) {
		/*
		 * Special case if there are also no power GPIOs.
		 * We are faking power and reset control with the
		 * power LED, so continue and blink the LED for reset.
		 */
		if (ARRAY_SIZE(power_gpios) > 0) {
			LOG_INF("No reset GPIO");
			return -ENOTSUP;
		}
	}

	ret = host_power_lock();
	if (ret < 0)
		return ret;

	state = host_power_state();
	if (state == 0) {
		LOG_ERR("Host reset requested with power off\n");
		ret = -1;
		goto out;
	}

	/* A reset pulse does nothing to a host that is off, so send it blind */
	if (state < 0)
		LOG_WRN("Host power state unknown; pulsing reset anyway");

	if (DT_NODE_HAS_STATUS_OKAY(GPIO_RESET) &&
	    gpio_is_ready_dt(&gpio_reset)) {
		ret = gpio_pin_set_dt(&gpio_reset, 1);
		if (ret < 0) {
			LOG_INF("Could not assert RESET GPIO\n");
			ret = -1;
			goto out;
		}
	}

	if (DT_NODE_HAS_STATUS_OKAY(GPIO_POWER_LED) &&
	    gpio_is_ready_dt(&power_led_gpio)) {
		for (int i = 0; i < 10; i++) {
			gpio_pin_set_dt(&power_led_gpio, 0);
			k_msleep(50);
			gpio_pin_set_dt(&power_led_gpio, 1);
			k_msleep(50);
		}
	} else {
		k_msleep(1000);
	}

	if (DT_NODE_HAS_STATUS_OKAY(GPIO_RESET) &&
	    gpio_is_ready_dt(&gpio_reset)) {
		ret = gpio_pin_set_dt(&gpio_reset, 0);
		if (ret < 0) {
			LOG_INF("Could not deassert RESET GPIO\n");
			ret = -1;
			goto out;
		}
	}

	ret = 0;
out:
	k_mutex_unlock(&host_power_mutex);
	return ret;
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!is_boot_finished()) {
		shell_error(sh, "must wait for boot to finish");
		return -EAGAIN;
	}

	rc = power_reset();
	if (rc)
		shell_error(sh, "reset failed (err=%d)", rc);

	return rc;
}

SHELL_CMD_REGISTER(reset, NULL, "Reset.", cmd_reset);
