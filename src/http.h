/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __HTTP_H__
#define __HTTP_H__

#ifdef CONFIG_HTTP_SERVER
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>

int app_http_server_init(void);
struct http_request_ctx;
int ws_validate_auth(int ws_socket, struct http_request_ctx *request_ctx, void *user_data);

/*
 * Define a resource on both the HTTP (port 80) and HTTPS (port 443) services.
 *
 * With CONFIG_APP_HTTP=n the HTTP service doesn't serve the resource, it
 * redirects the request to the same URL over HTTPS instead.
 */
#if defined(CONFIG_APP_HTTP)
#define HTTP_ONLY_RESOURCE_DEFINE(name, url, detail)					\
HTTP_RESOURCE_DEFINE(name##_http, http_service, url, detail);
#else
extern struct http_resource_detail_dynamic https_redirect_detail;
#define HTTP_ONLY_RESOURCE_DEFINE(name, url, detail)					\
HTTP_RESOURCE_DEFINE(name##_http, http_service, url, &https_redirect_detail);
#endif

#if defined(CONFIG_APP_HTTPS)
#define HTTPS_ONLY_RESOURCE_DEFINE(name, url, detail)					\
HTTP_RESOURCE_DEFINE(name##_https, https_service, url, detail);
#else
#define HTTPS_ONLY_RESOURCE_DEFINE(name, url, detail)
#endif

#define ALL_HTTP_RESOURCE_DEFINE(name, url, detail)					\
HTTP_ONLY_RESOURCE_DEFINE(name, url, detail)						\
HTTPS_ONLY_RESOURCE_DEFINE(name, url, detail)

#else
struct http_request_ctx;
static inline int app_http_server_init(void) { return 0; }
static inline int ws_validate_auth(int ws_socket, struct http_request_ctx *request_ctx, void *user_data) { return -EACCES; }
#endif

#endif
