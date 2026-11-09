/*
 * HTTP web server
 *
 * Copyright (c) 2023, Emna Rekik
 * Copyright (c) 2024, Nordic Semiconductor
 *
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#if defined(CONFIG_SHELL_BACKEND_WEBSOCKET)
#include <zephyr/shell/shell_websocket.h>
#endif
#include <stdio.h>
#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wallabmc_http, LOG_LEVEL_INF);

#include "http.h"
#include "https_cert.h"
#include "config.h"

BUILD_ASSERT(IS_ENABLED(CONFIG_APP_HTTP) || IS_ENABLED(CONFIG_APP_HTTPS),
	     "CONFIG_APP_HTTP=n requires CONFIG_APP_HTTPS");

static uint16_t http_service_port = 80;
HTTP_SERVICE_DEFINE(http_service, NULL, &http_service_port,
		    3, 10, NULL, NULL, NULL);

#if defined(CONFIG_APP_HTTP)
#if defined(CONFIG_SHELL_BACKEND_WEBSOCKET)
DEFINE_WEBSOCKET_SERVICE(http_service);
#endif
#else /* defined(CONFIG_APP_HTTP) */
/*
 * Plain HTTP doesn't serve anything, resources defined with
 * ALL_HTTP_RESOURCE_DEFINE() redirect to the same URL over HTTPS. The Host
 * header is used so the redirect keeps the name the client used for the BMC.
 */
HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_host, "host");

static int https_redirect_location(struct http_client_ctx *client,
				   const struct http_request_ctx *request_ctx,
				   char *buf, size_t len)
{
	const char *host = NULL;
	size_t host_len;
	const char *p;
	int ret;

	for (size_t i = 0; i < request_ctx->header_count; i++) {
		if (!strcmp(request_ctx->headers[i].name, "host")) {
			host = request_ctx->headers[i].value;
			break;
		}
	}

	if (!host || !host[0])
		return -EINVAL;

	/* Drop any port, but not the colons in an IPv6 literal ("[::1]:80") */
	host_len = strlen(host);
	p = strrchr(host, ':');
	if (p && !strchr(p, ']'))
		host_len = p - host;

	ret = snprintf(buf, len, "https://%.*s%s", (int)host_len, host,
		       (const char *)client->url_buffer);
	if (ret < 0 || (size_t)ret >= len)
		return -ENOMEM;

	return 0;
}

static int https_redirect_cb(struct http_client_ctx *client,
			     enum http_transaction_status status,
			     const struct http_request_ctx *request_ctx,
			     struct http_response_ctx *response_ctx,
			     void *user_data)
{
	/*
	 * The server serialises requests by resource detail, which is shared
	 * by all the redirect resources, so static data can be used here.
	 */
	static char location[sizeof("https://") + CONFIG_HTTP_SERVER_MAX_HEADER_LEN +
			     CONFIG_HTTP_SERVER_MAX_URL_LENGTH];
	static struct http_header location_header = {
		.name = "Location",
		.value = location,
	};
	static bool responded;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		responded = false;
		return 0;
	}

	/*
	 * Captured headers are only passed in the first callback, so respond
	 * immediately and ignore any request body that follows.
	 */
	if (responded)
		return 0;
	responded = true;

	if (https_redirect_location(client, request_ctx, location, sizeof(location))) {
		response_ctx->status = HTTP_400_BAD_REQUEST;
	} else {
		/* 308 so clients repeat non-GET methods and bodies */
		response_ctx->status = HTTP_308_PERMANENT_REDIRECT;
		response_ctx->headers = &location_header;
		response_ctx->header_count = 1;
	}
	response_ctx->final_chunk = true;

	return 0;
}

struct http_resource_detail_dynamic https_redirect_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		/* Not HEAD, the server answers that itself without the callback */
		.bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_POST) |
						     BIT(HTTP_PUT) | BIT(HTTP_PATCH) |
						     BIT(HTTP_DELETE),
		.content_type = "text/plain",
	},
	.cb = https_redirect_cb,
};
#endif /* defined(CONFIG_APP_HTTP) */

#if defined(CONFIG_APP_HTTPS)

#ifndef CONFIG_NET_SOCKETS_SOCKOPT_TLS
#error "CONFIG_NET_SOCKETS_SOCKOPT_TLS=n"
#endif

#if defined(CONFIG_APP_HTTPS_PSK)
#ifndef CONFIG_MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#error "CONFIG_MBEDTLS_KEY_EXCHANGE_PSK_ENABLED=n"
#endif
#endif

enum tls_tag {
#if defined(CONFIG_APP_HTTPS_PUBLIC_KEY)
        /** Used for both the public and private server keys */
        HTTP_SERVER_CERTIFICATE_TAG,
#endif
#if defined(CONFIG_APP_HTTPS_PSK)
        PSK_TAG,
#endif
};

static const sec_tag_t sec_tag_list_verify_none[] = {
#if defined(CONFIG_APP_HTTPS_PUBLIC_KEY)
		HTTP_SERVER_CERTIFICATE_TAG,
#endif
#if defined(CONFIG_APP_HTTPS_PSK)
		PSK_TAG,
#endif
	};

static uint16_t https_service_port = 443;
HTTPS_SERVICE_DEFINE(https_service, NULL, &https_service_port,
		     3, 10, NULL, NULL, NULL, sec_tag_list_verify_none,
		     sizeof(sec_tag_list_verify_none));

#if defined(CONFIG_SHELL_BACKEND_WEBSOCKET)
DEFINE_WEBSOCKET_SERVICE(https_service);
#endif

static int setup_tls(void)
{
	int err = 0;

#if defined(CONFIG_APP_HTTPS_PUBLIC_KEY)
	const uint8_t *cert, *key;
	size_t cert_len, key_len;

	/*
	 * Without a certificate nothing is registered under the tag, and the
	 * HTTP server doesn't start the HTTPS service. Port 80 still starts.
	 */
	if (https_cert_init() == 0 &&
	    https_cert_get(&cert, &cert_len, &key, &key_len) == 0) {
		err = tls_credential_add(HTTP_SERVER_CERTIFICATE_TAG,
					 TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
					 cert, cert_len);
		if (err < 0) {
			LOG_ERR("Failed to register public certificate: %d", err);
			return err;
		}

		err = tls_credential_add(HTTP_SERVER_CERTIFICATE_TAG,
					 TLS_CREDENTIAL_PRIVATE_KEY,
					 key, key_len);
		if (err < 0) {
			LOG_ERR("Failed to register private key: %d", err);
			return err;
		}
	}
#endif

#if defined(CONFIG_APP_HTTPS_PSK)
	const char *psk;
	int psk_len;
	if (config_bmc_https_psk(&psk, &psk_len)) {
		err = tls_credential_add(PSK_TAG,
					 TLS_CREDENTIAL_PSK,
					 psk,
					 psk_len);
		if (err < 0) {
			LOG_ERR("Failed to register PSK: %d", err);
			return err;
		}

		static const char psk_id[] = "WallaBMC";
		err = tls_credential_add(PSK_TAG,
					 TLS_CREDENTIAL_PSK_ID,
					 psk_id,
					 sizeof(psk_id) - 1);
		if (err < 0) {
			LOG_ERR("Failed to register PSK ID: %d", err);
			return err;
		}
	}
#endif

	return err;
}
#else /* defined(CONFIG_APP_HTTPS) */
static int setup_tls(void)
{
	return 0;
}
#endif /* defined(CONFIG_APP_HTTPS) */

#include <zephyr/net/websocket.h>
#define CREDENTIALS_MAX_LEN 64
int ws_validate_auth(int ws_socket, struct http_request_ctx *request_ctx, void *user_data)
{
	static const char auth_prefix[] = "Auth:";
	uint8_t rx_buf[sizeof(auth_prefix) + CREDENTIALS_MAX_LEN];
	uint32_t message_type;
	uint64_t remaining;
	int rc;
	int32_t timeout_ms = 3000;

	rc = websocket_recv_msg(ws_socket, rx_buf, sizeof(rx_buf) - 1,
				&message_type, &remaining, timeout_ms);
	if (rc <= 0) {
		LOG_WRN("No websocket auth message (err=%d)", rc);
		websocket_disconnect(ws_socket);
		return -EIO;
	}

	rx_buf[rc] = '\0'; /* Nul terminate so we can strcmp it */

	if (memcmp(rx_buf, auth_prefix, sizeof(auth_prefix) - 1)) {
		LOG_WRN("No websocket auth message (%s)", rx_buf);
		websocket_disconnect(ws_socket);
		return -EINVAL;
	}

	const char *auth = rx_buf + sizeof(auth_prefix) - 1;

	uint8_t expected[CREDENTIALS_MAX_LEN];
	snprintf(expected, sizeof(expected), "%s_%s", "admin", config_bmc_admin_password());

	if (strcmp(auth, expected)) {
		LOG_WRN("Websocket auth incorrect (%s)", rx_buf);
		websocket_disconnect(ws_socket);
		return -EPERM;
	}

	return 0;
}

#if defined(CONFIG_APP_HTTP)
static int (*shell_http_ws_cb)(int ws_socket, struct http_request_ctx *request_ctx, void *user_data);

int shell_http_ws_auth_cb(int ws_socket, struct http_request_ctx *request_ctx, void *user_data)
{
	int rc = ws_validate_auth(ws_socket, request_ctx, user_data);
	if (rc < 0)
		return rc;

	return shell_http_ws_cb(ws_socket, request_ctx, user_data);
}
#endif

static int (*shell_https_ws_cb)(int ws_socket, struct http_request_ctx *request_ctx, void *user_data);

int shell_https_ws_auth_cb(int ws_socket, struct http_request_ctx *request_ctx, void *user_data)
{
	int rc = ws_validate_auth(ws_socket, request_ctx, user_data);
	if (rc < 0)
		return rc;

	return shell_https_ws_cb(ws_socket, request_ctx, user_data);
}

int app_http_server_init(void)
{
	int err;

	err = setup_tls();
	if (err)
		return err;

#if defined(CONFIG_SHELL_BACKEND_WEBSOCKET)
#if defined(CONFIG_APP_HTTP)
	shell_http_ws_cb = GET_WS_DETAIL_NAME(http_service).cb;
	GET_WS_DETAIL_NAME(http_service).cb = shell_http_ws_auth_cb;
	err = shell_websocket_enable(&GET_WS_SHELL_NAME(http_service));
	if (err)
		return err;
#endif
#if defined(CONFIG_APP_HTTPS)
	shell_https_ws_cb = GET_WS_DETAIL_NAME(https_service).cb;
	GET_WS_DETAIL_NAME(https_service).cb = shell_https_ws_auth_cb;
	err = shell_websocket_enable(&GET_WS_SHELL_NAME(https_service));
	if (err)
		return err;
#endif
#endif

	return http_server_start();
}
