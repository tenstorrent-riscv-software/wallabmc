/* Wi-Fi station bring-up using compile-time credentials. */

/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wallabmc_wifi, LOG_LEVEL_INF);

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/shell/shell.h>

#include "config.h"
#include "wifi.h"

#define WIFI_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | \
			  NET_EVENT_WIFI_DISCONNECT_RESULT)

static struct net_mgmt_event_callback wifi_cb;
static K_SEM_DEFINE(wifi_disconnected_sem, 0, 1);

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event,
			       struct net_if *iface)
{
	const struct wifi_status *status = cb->info;

	ARG_UNUSED(iface);

	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		if (status && status->status) {
			LOG_ERR("Wi-Fi connect failed (status=%d)", status->status);
		} else {
			LOG_INF("Wi-Fi connected");
		}
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		LOG_WRN("Wi-Fi disconnected");
		k_sem_give(&wifi_disconnected_sem);
		break;
	default:
		break;
	}
}

/*
 * Connect using a runtime-supplied SSID/password pair, bypassing the
 * compile-time CONFIG_WIFI_CREDENTIALS_STATIC entry in the
 * wifi_credentials store. Used at boot when the user has saved
 * credentials via `wifi connect`, and from the shell command itself.
 *
 * We avoid wifi_credentials_set_personal() + NET_REQUEST_WIFI_CONNECT_STORED
 * here because that path also re-adds the static entry whenever the
 * stored SSID does not match it, which races with our update on the
 * ESP32-C6 build and intermittently wedges Wi-Fi association after a
 * disconnect+reconnect.
 */
static int wifi_connect_with(struct net_if *iface, const char *ssid, const char *password)
{
	static uint8_t ssid_buf[WIFI_SSID_MAX_LEN + 1];
	static uint8_t psk_buf[WIFI_PSK_MAX_LEN + 1];
	struct wifi_connect_req_params params = {
		.ssid = ssid_buf,
		.psk = psk_buf,
	};
	size_t ssid_len = strlen(ssid);
	size_t psk_len = strlen(password);

	if (ssid_len == 0 || ssid_len > WIFI_SSID_MAX_LEN)
		return -EINVAL;
	if (psk_len > WIFI_PSK_MAX_LEN)
		return -EINVAL;

	memcpy(ssid_buf, ssid, ssid_len);
	params.ssid_length = ssid_len;
	memcpy(psk_buf, password, psk_len);
	params.psk_length = psk_len;
	params.security = (psk_len > 0) ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE;
	params.channel = WIFI_CHANNEL_ANY;
	params.band = WIFI_FREQ_BAND_UNKNOWN;
	params.mfp = WIFI_MFP_OPTIONAL;
	params.timeout = SYS_FOREVER_MS;

	return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
}

int wifi_connect_init(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_ps_params ps = {
		.enabled = WIFI_PS_DISABLED,
	};
	const char *ssid = config_wifi_ssid();
	int rc;

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
				     WIFI_MGMT_EVENTS);
	net_mgmt_add_event_callback(&wifi_cb);

	/*
	 * The ESP32 driver enables WIFI_PS_MAX_MODEM by default, which causes
	 * 100+ ms RX latency spikes and dropped frames under bursty HTTP load.
	 * The BMC is mains-powered, so trade the power saving for reliability.
	 */
	rc = net_mgmt(NET_REQUEST_WIFI_PS, iface, &ps, sizeof(ps));
	if (rc) {
		LOG_WRN("Could not disable Wi-Fi power save (rc=%d)", rc);
	}

	if (ssid[0] != '\0') {
		LOG_INF("Connecting to stored Wi-Fi SSID: %s", ssid);
		rc = wifi_connect_with(iface, ssid, config_wifi_password());
		if (rc) {
			LOG_ERR("Wi-Fi connect request failed (rc=%d)", rc);
			return rc;
		}
		LOG_INF("Wi-Fi connect requested; waiting for association...");
		return 0;
	}

#if defined(CONFIG_WIFI_CREDENTIALS_CONNECT_STORED)
	/*
	 * NVS has no SSID; fall back to whatever is in the wifi_credentials
	 * store (typically the compile-time CONFIG_WIFI_CREDENTIALS_STATIC
	 * entry baked into the image).
	 */
	rc = net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, iface, NULL, 0);
	if (rc) {
		LOG_ERR("Wi-Fi connect-stored request failed (rc=%d)", rc);
		return rc;
	}
	LOG_INF("Wi-Fi connect requested; waiting for association...");
#else
	LOG_WRN("No Wi-Fi credentials configured -- use `wifi connect <ssid> <password>` to set them");
#endif
	return 0;
}

void wifi_shutdown(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	int rc;

	if (!iface)
		return;

	/*
	 * sys_reboot() on ESP32 leaves the Wi-Fi MAC/AP-side association in
	 * a state where the next boot sometimes fails to re-associate (board
	 * stays unreachable, no DHCP). Disassociate, wait for the driver to
	 * confirm via NET_EVENT_WIFI_DISCONNECT_RESULT, then take the iface
	 * down so the AP drops us cleanly before the reset.
	 */
	k_sem_reset(&wifi_disconnected_sem);
	rc = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	if (rc && rc != -EALREADY) {
		LOG_WRN("Wi-Fi disconnect request failed (rc=%d)", rc);
	} else if (rc == 0) {
		(void)k_sem_take(&wifi_disconnected_sem, K_MSEC(1000));
	}

	rc = net_if_down(iface);
	if (rc && rc != -EALREADY) {
		LOG_WRN("Wi-Fi iface down failed (rc=%d)", rc);
	}
	k_msleep(200);
}

static int cmd_wifi_connect(const struct shell *sh, size_t argc, char **argv)
{
	struct net_if *iface = net_if_get_first_wifi();
	const char *ssid;
	const char *password;
	int rc;

	ARG_UNUSED(argc);

	ssid = argv[1];
	password = argv[2];

	if (!iface) {
		shell_error(sh, "No Wi-Fi interface found");
		return -ENODEV;
	}

	rc = config_wifi_set(ssid, password);
	if (rc) {
		shell_error(sh, "Could not save Wi-Fi credentials (err=%d)", rc);
		return rc;
	}

	/*
	 * Disassociate from the current AP (best effort -- ignore -EALREADY)
	 * and wait briefly for the driver to confirm before reconnecting
	 * with the new credentials.
	 */
	k_sem_reset(&wifi_disconnected_sem);
	rc = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	if (rc == 0)
		(void)k_sem_take(&wifi_disconnected_sem, K_MSEC(1000));

	rc = wifi_connect_with(iface, ssid, password);
	if (rc) {
		shell_error(sh, "Wi-Fi connect failed (err=%d)", rc);
		return rc;
	}

	shell_info(sh, "Wi-Fi credentials saved; connecting to %s", ssid);
	return 0;
}

static int cmd_wifi_clear(const struct shell *sh, size_t argc, char **argv)
{
	struct net_if *iface = net_if_get_first_wifi();
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = config_wifi_clear();
	if (rc) {
		shell_error(sh, "Could not clear Wi-Fi credentials (err=%d)", rc);
		return rc;
	}

	if (iface) {
		k_sem_reset(&wifi_disconnected_sem);
		rc = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
		if (rc == 0)
			(void)k_sem_take(&wifi_disconnected_sem, K_MSEC(1000));
	}

	shell_info(sh, "Wi-Fi credentials cleared; not reconnecting");
	return 0;
}

static int cmd_wifi_config(const struct shell *sh, size_t argc, char **argv)
{
	const char *ssid = config_wifi_ssid();
	const char *password = config_wifi_password();

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (ssid[0] == '\0') {
		shell_print(sh, "Wi-Fi: no credentials stored (use `wifi connect <ssid> <password>`)");
	} else {
		shell_print(sh, "Wi-Fi SSID:     %s", ssid);
		shell_print(sh, "Wi-Fi password: %s", password);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_wifi_cmds,
	SHELL_CMD_ARG(connect, NULL,
		      "Connect to a Wi-Fi network and persist the credentials.\n"
		      "Usage: wifi connect <ssid> <password>",
		      cmd_wifi_connect, 3, 0),
	SHELL_CMD(clear,  NULL, "Clear the stored Wi-Fi credentials and disconnect.", cmd_wifi_clear),
	SHELL_CMD(config, NULL, "Show the stored Wi-Fi credentials.",                  cmd_wifi_config),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(wifi, &sub_wifi_cmds, "Wi-Fi commands", NULL);
