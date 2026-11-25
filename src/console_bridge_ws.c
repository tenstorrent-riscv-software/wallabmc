/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(console_ws, LOG_LEVEL_INF);

#include <zephyr/posix/fcntl.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/socket_service.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/websocket.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <string.h>

#include "console_logger.h"
#include "http.h"

/*
 * Receive buffer size.
 * May not be much benefit to being larger than UART_TX_BUF_SIZE
 */
#define WS_RX_BUF_SIZE 32

/* One websocket endpoint per console, one client at a time */
struct console_ws {
	const char *endpoint;
	uint16_t ord;		/* devicetree ordinal, matches struct console */
	struct console *c;
	const struct net_socket_service_desc *svc;
	/** Array for sockets used by the websocket service. */
	struct zsock_pollfd fds[1];
	bool new_client;
	struct k_thread sender;
};

static void ws_server_cb(struct net_socket_service_event *evt);
static int console_ws_http_cb(int ws_socket, struct http_request_ctx *request_ctx,
			      void *user_data);

/*
 * The socket service and HTTP resources need one static definition per
 * console, so they are generated from the devicetree; the node's
 * dependency ordinal makes the names unique.
 */
#define WS_ID(what, node) UTIL_CAT(console_ws_##what##_, DT_DEP_ORD(node))

#define CONSOLE_WS_DEFINE(node)							\
	NET_SOCKET_SERVICE_SYNC_DEFINE_STATIC(WS_ID(svc, node), ws_server_cb, 1); \
	static struct console_ws WS_ID(ctx, node) = {				\
		.endpoint = DT_PROP(node, endpoint),				\
		.ord = DT_DEP_ORD(node),					\
		.svc = &WS_ID(svc, node),					\
		.fds = { { .fd = -1 } },					\
	};									\
	BUILD_ASSERT(sizeof(DT_PROP(node, label)) +				\
		     sizeof(DT_PROP(node, endpoint)) + 27 <= 96,		\
		     "console label and endpoint too long for /consoles");	\
	/*									\
	 * Not sure how to size this, the http server docs example has	\
	 * 1024 bytes, but 256 seems to be okay.				\
	 */									\
	static uint8_t WS_ID(buf, node)[256];					\
	static struct http_resource_detail_websocket WS_ID(detail, node) = {	\
		.common = {							\
			.type = HTTP_RESOURCE_TYPE_WEBSOCKET,			\
			/* We need HTTP/1.1 GET method for upgrading */	\
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),	\
		},								\
		.cb = console_ws_http_cb,					\
		.data_buffer = WS_ID(buf, node),				\
		.data_buffer_len = sizeof(WS_ID(buf, node)),			\
		.user_data = &WS_ID(ctx, node),					\
	};									\
	ALL_HTTP_RESOURCE_DEFINE(WS_ID(res, node), DT_PROP(node, endpoint),	\
				 &WS_ID(detail, node))

DT_FOREACH_STATUS_OKAY(wallabmc_console, CONSOLE_WS_DEFINE)

#define CONSOLE_WS_PTR(node) &WS_ID(ctx, node),

static struct console_ws *const wss[] = {
	DT_FOREACH_STATUS_OKAY(wallabmc_console, CONSOLE_WS_PTR)
};

BUILD_ASSERT(ARRAY_SIZE(wss) == NCONSOLE);

K_THREAD_STACK_ARRAY_DEFINE(ws_stacks, NCONSOLE, CONFIG_CONSOLE_BRIDGE_WS_STACK_SIZE);

static void ws_end_client_connection(struct console_ws *ws)
{
	LOG_INF("%s console: closing websocket #%d", ws->c->name, ws->fds[0].fd);

	(void)net_socket_service_unregister(ws->svc);
	(void)websocket_unregister(ws->fds[0].fd);

	ws->fds[0].fd = -1;
}

static ssize_t ws_send(struct console_ws *ws, const void *buf, size_t size, bool block)
{
	int fd = ws->fds[0].fd;
	size_t copied = 0;
	ssize_t ret;

	if (fd < 0) {
		return -ENOTCONN;
	}

	while (copied < size) {
		/*
		 * Binary format should be used because we can't control what the host
		 * sends, and text requires valid UTF-8 or the websocket connection gets
		 * closed.
		 */
		ret = websocket_send_msg(fd, (uint8_t *)buf + copied, size - copied,
					 WEBSOCKET_OPCODE_DATA_BINARY, false, true,
					 block ? SYS_FOREVER_MS : 0);
		if (ret < 0) {
			if (errno == EAGAIN && !block)
				return copied;
			if (copied)
				ret = copied;
			else
				ret = -errno;
			LOG_ERR("Failed to send (err %zd), shutting down", ret);
			/* Only this client: a new one may have taken its place */
			if (ws->fds[0].fd == fd)
				ws_end_client_connection(ws);
			return ret;
		}

		copied += ret;
	}

	return copied;
}

static ssize_t ws_recv(struct console_ws *ws, void *buf, size_t size, bool block)
{
	int fd = ws->fds[0].fd;
	ssize_t ret;
	uint32_t message_type;
	uint64_t remaining;

	ret = websocket_recv_msg(fd, buf, size,
				 &message_type, &remaining, block ? SYS_FOREVER_MS : 0);
	if (ret < 0) {
		LOG_DBG("Websocket client error %d", ret);
		if (errno == EAGAIN && !block)
			return -EAGAIN;
		if (ws->fds[0].fd == fd)
			ws_end_client_connection(ws);
		return -errno;
	} else if (ret == 0) {
		LOG_DBG("Websocket client closed connection");
		if (ws->fds[0].fd == fd)
			ws_end_client_connection(ws);
		return 0;
	}

	return ret;
}

static void sender_thread(void *a, void *unused1, void *unused2)
{
	struct console_ws *ws = a;
	struct console *c = ws->c;
	uint64_t pos = 0;
	uint8_t buf[64];

	while (1) {
		ssize_t nr, ret;
		size_t copied;

		if (ws->fds[0].fd == -1) {
			k_event_wait_safe(&c->events, CONSOLE_EVENT_WS_CLIENT, false, K_FOREVER);
			continue;
		}

		if (ws->new_client) {
			ws->new_client = false;
			pos = 0;
		}

		ret = console_read(c, buf, sizeof(buf), &pos);
		if (ret < 0) {
			pos = 0;
			continue;
		}
		if (ret == 0) {
			k_event_wait_safe(&c->events,
					  CONSOLE_EVENT_WS_CLIENT | CONSOLE_EVENT_DATA_WS,
					  false, K_FOREVER);
			continue;
		}

		nr = ret;
		copied = 0;
again:
		if (ws->fds[0].fd == -1)
			continue;

		ret = ws_send(ws, buf + copied, nr - copied, true);
		if (ret < 0) {
			LOG_WRN("Socket send error: %d ret: %zd", errno, ret);
			continue;
		}

		copied += ret;
		if (copied < nr) {
			LOG_WRN("Socket send short: %d ret: %zd", errno, ret);
			k_msleep(50);
			goto again;
		}
	}
}

static uint8_t ws_recv_buf[WS_RX_BUF_SIZE];

static void ws_server_cb(struct net_socket_service_event *evt)
{
	net_socklen_t optlen = sizeof(int);
	struct console_ws *ws;
	int sock_error;

	ws = (struct console_ws *)evt->user_data;

	if ((evt->event.revents & ZSOCK_POLLERR) ||
	    (evt->event.revents & ZSOCK_POLLNVAL)) {
		(void)zsock_getsockopt(evt->event.fd, ZSOCK_SOL_SOCKET,
				       ZSOCK_SO_ERROR, &sock_error, &optlen);
		LOG_ERR("Websocket socket %d error (%d)", evt->event.fd, sock_error);

		if (evt->event.fd == ws->fds[0].fd) {
			return ws_end_client_connection(ws);
		}

		return;
	}

	if (!(evt->event.revents & ZSOCK_POLLIN)) {
		return;
	}

	if (evt->event.fd == ws->fds[0].fd) {
		ssize_t ret;

		ret = ws_recv(ws, ws_recv_buf, sizeof(ws_recv_buf), false);
		if (ret <= 0)
			return;

		console_write(ws->c, ws_recv_buf, ret);
	}
}

static int console_ws_http_cb(int ws_socket, struct http_request_ctx *request_ctx,
			      void *user_data)
{
	struct console_ws *ctx = user_data;
	int ret;

	if (ws_socket < 0) {
		LOG_ERR("Invalid socket %d", ws_socket);
		return -EBADF;
	}

	if (!ctx->c) {
		LOG_ERR("%s: console not running", ctx->endpoint);
		return -ENODEV;
	}

	ret = ws_validate_auth(ws_socket, request_ctx, user_data);
	if (ret < 0)
		return ret;

	if (ctx->fds[0].fd >= 0) {
		/* There is already a websocket connection to this console,
		 * kick the previous connection out.
		 */
		ws_end_client_connection(ctx);
	}

	ctx->fds[0].fd = ws_socket;
	ctx->fds[0].events = ZSOCK_POLLIN;
	ctx->new_client = true;

	ret = net_socket_service_register(ctx->svc, ctx->fds, ARRAY_SIZE(ctx->fds), ctx);
	if (ret < 0) {
		LOG_ERR("Failed to register socket service, %d", ret);
		goto error;
	}

	LOG_INF("%s console: websocket client connected", ctx->c->name);
	k_event_post(&ctx->c->events, CONSOLE_EVENT_WS_CLIENT);

	return 0;

error:
	if (ctx->fds[0].fd >= 0) {
		(void)zsock_close(ctx->fds[0].fd);
		ctx->fds[0].fd = -1;
	}

	return ret;
}

/* GET /consoles: the running consoles for the web UI, as JSON */
static int consoles_handler(struct http_client_ctx *client,
			    enum http_transaction_status status,
			    const struct http_request_ctx *request_ctx,
			    struct http_response_ctx *response_ctx,
			    void *user_data)
{
	static const struct http_header headers[] = {
		{ .name = "content-type", .value = "application/json" },
		{ .name = "cache-control", .value = "no-cache" },
	};
	/* The HTTP server runs handlers on one thread and sends the body before returning */
	static char body[NCONSOLE * 96 + 4];
	size_t len = 0;
	bool first = true;
	int i;

	if (status == HTTP_SERVER_TRANSACTION_COMPLETE ||
	    status == HTTP_SERVER_TRANSACTION_ABORTED)
		return 0;

	len += snprintf(body + len, sizeof(body) - len, "[");
	for (i = 0; i < NCONSOLE; i++) {
		const struct console *c = &consoles[i];

		if (!c->ready)
			continue;
		len += snprintf(body + len, sizeof(body) - len,
				"%s{\"label\":\"%s\",\"endpoint\":\"%s\"}",
				first ? "" : ",", c->name, c->endpoint);
		len = MIN(len, sizeof(body) - 1);
		first = false;
	}
	len += snprintf(body + len, sizeof(body) - len, "]");
	len = MIN(len, sizeof(body) - 1);

	response_ctx->status = HTTP_200_OK;
	response_ctx->headers = headers;
	response_ctx->header_count = ARRAY_SIZE(headers);
	response_ctx->body = body;
	response_ctx->body_len = len;
	response_ctx->final_chunk = true;

	return 0;
}

static struct http_resource_detail_dynamic consoles_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = consoles_handler,
};
ALL_HTTP_RESOURCE_DEFINE(consoles, "/consoles", &consoles_detail);

int console_bridge_ws_init(void)
{
	int i, j;

	for (i = 0; i < NCONSOLE; i++) {
		struct console_ws *ws = wss[i];
		char name[24];

		for (j = 0; j < NCONSOLE; j++) {
			if (consoles[j].ord == ws->ord)
				break;
		}
		if (j == NCONSOLE || !consoles[j].ready)
			continue;
		ws->c = &consoles[j];

		k_thread_create(&ws->sender, ws_stacks[i],
				K_THREAD_STACK_SIZEOF(ws_stacks[i]),
				sender_thread, ws, NULL, NULL,
				CONFIG_CONSOLE_BRIDGE_WS_PRIORITY, 0, K_NO_WAIT);
		snprintf(name, sizeof(name), "%s ws", ws->c->name);
		k_thread_name_set(&ws->sender, name);
		LOG_INF("%s console: websocket at %s", ws->c->name, ws->endpoint);
	}

	return 0;
}
