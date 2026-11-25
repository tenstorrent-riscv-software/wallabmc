/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(console_bridge, LOG_LEVEL_INF);

#include <zephyr/posix/fcntl.h>
#include <zephyr/net/socket.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <string.h>

#include "console_logger.h"

/* One raw TCP server per console: an accept loop and a sender thread */
struct bridge {
	struct console *c;
	volatile int client_fd;
	volatile bool new_client;
	struct k_thread daemon;
	struct k_thread sender;
};

static struct bridge bridges[NCONSOLE];

K_THREAD_STACK_ARRAY_DEFINE(daemon_stacks, NCONSOLE, CONFIG_CONSOLE_BRIDGE_STACK_SIZE);
K_THREAD_STACK_ARRAY_DEFINE(sender_stacks, NCONSOLE, CONFIG_CONSOLE_BRIDGE_STACK_SIZE);

static void sender_thread(void *a, void *unused1, void *unused2)
{
	struct bridge *b = a;
	struct console *c = b->c;
	uint64_t pos = 0;
	uint8_t buf[64];

	while (1) {
		ssize_t ret;
		int fd;

		if (b->client_fd == -1) {
			k_event_wait_safe(&c->events, CONSOLE_EVENT_TCP_CLIENT, false, K_FOREVER);
			continue;
		}

		if (b->new_client) {
			/*
			 * Whereas websocket clients get all history, TCP
			 * clients only see subsequent console output.
			 */
			console_seek_end(c, &pos);
			b->new_client = false;
		}

		ret = console_read(c, buf, sizeof(buf), &pos);
		if (ret < 0) {
			console_seek_end(c, &pos);
			continue;
		}
		if (ret == 0) {
			k_event_wait_safe(&c->events,
					  CONSOLE_EVENT_TCP_CLIENT | CONSOLE_EVENT_DATA_TCP,
					  false, K_FOREVER);
			continue;
		}

		fd = b->client_fd;
		if (fd == -1)
			continue;

		ret = send(fd, buf, ret, 0);
		if (ret <= 0) {
			LOG_WRN("%s console: send error: %d len: %zd", c->name, errno, ret);
			/* Only this client: the accept loop may have replaced it */
			if (b->client_fd == fd) {
				shutdown(fd, 2);
				b->client_fd = -1;
			}
		}
	}
}

static void handle_client(struct bridge *b, int client_fd)
{
	struct console *c = b->c;
	uint8_t socket_buf[64];
	int opt = 1;

	/* Disable Nagle so typed characters go out at once */
	if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
		LOG_WRN("%s console: failed to set TCP_NODELAY: %d", c->name, errno);
	}

	LOG_INF("%s console: client connected on TCP port %u", c->name, c->port);

	b->client_fd = client_fd;
	b->new_client = true;
	k_event_post(&c->events, CONSOLE_EVENT_TCP_CLIENT);

	while (b->client_fd == client_fd) {
		ssize_t rc = recv(client_fd, socket_buf, sizeof(socket_buf), 0);
		if (rc <= 0) {
			if (rc == 0) {
				LOG_INF("%s console: client disconnected", c->name);
			} else {
				LOG_WRN("%s console: recv error: %d", c->name, errno);
			}
			shutdown(client_fd, 2);
			if (b->client_fd == client_fd)
				b->client_fd = -1;
			break;
		}

		console_write(c, socket_buf, rc);
	}
}

static void daemon_thread(void *a, void *unused1, void *unused2)
{
	struct bridge *b = a;
	struct console *c = b->c;
	struct sockaddr_in server_addr;
	int server_fd;
	int opt;

	server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server_fd < 0) {
		LOG_ERR("%s console: socket() failed: %d", c->name, errno);
		return;
	}

	opt = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
		LOG_WRN("%s console: failed to set SO_REUSEADDR: %d", c->name, errno);
	}

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(c->port);
	server_addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		LOG_ERR("%s console: bind() failed: %d", c->name, errno);
		close(server_fd);
		return;
	}

	if (listen(server_fd, 1) < 0) {
		LOG_ERR("%s console: listen() failed: %d", c->name, errno);
		close(server_fd);
		return;
	}

	LOG_INF("%s console: TCP bridge listening on port %u", c->name, c->port);

	while (1) {
		struct sockaddr_in client_addr;
		socklen_t client_addr_len = sizeof(client_addr);
		int client_fd;

		client_fd = accept(server_fd, (struct sockaddr *)&client_addr,
				   &client_addr_len);
		if (client_fd < 0) {
			LOG_ERR("%s console: accept() failed: %d", c->name, errno);
			continue;
		}

		handle_client(b, client_fd);
		close(client_fd);
	}
}

int console_bridge_init(void)
{
	int i;

	for (i = 0; i < NCONSOLE; i++) {
		struct bridge *b = &bridges[i];
		char name[24];

		b->c = &consoles[i];
		b->client_fd = -1;
		if (!b->c->ready)
			continue;

		k_thread_create(&b->daemon, daemon_stacks[i],
				K_THREAD_STACK_SIZEOF(daemon_stacks[i]),
				daemon_thread, b, NULL, NULL,
				CONFIG_CONSOLE_BRIDGE_PRIORITY, 0, K_NO_WAIT);
		snprintf(name, sizeof(name), "%s bridge", b->c->name);
		k_thread_name_set(&b->daemon, name);

		k_thread_create(&b->sender, sender_stacks[i],
				K_THREAD_STACK_SIZEOF(sender_stacks[i]),
				sender_thread, b, NULL, NULL,
				CONFIG_CONSOLE_BRIDGE_PRIORITY, 0, K_NO_WAIT);
		snprintf(name, sizeof(name), "%s send", b->c->name);
		k_thread_name_set(&b->sender, name);
	}

	return 0;
}
