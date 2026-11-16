/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * OpenOCD remote_bitbang servers, one per wallabmc,jtag node. Each chain
 * has its own TCP port and thread. Its pins float until a client
 * connects, and the board mux, when the node has one, points the chain
 * at the MCU only for the length of the connection.
 */
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wallabmc_jtag, LOG_LEVEL_INF);

#include <zephyr/posix/fcntl.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/socket.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>

#define JTAG_MAX_SELECT 4
#define JTAG_CMD_BUF	64

struct jtag {
	const char *name;
	uint16_t port;
	struct gpio_dt_spec tck;
	struct gpio_dt_spec tms;
	struct gpio_dt_spec tdi;
	struct gpio_dt_spec tdo;
	struct gpio_dt_spec trst;	/* port NULL without a trst line */
	struct gpio_dt_spec select[JTAG_MAX_SELECT];
	uint8_t nselect;
	uint8_t select_mcu;
	int8_t select_idle;		/* -1 leaves the lines as inputs */
	int tck_prev;
	int tms_prev;
	int tdi_prev;
	bool srst_warned;
	bool swd_warned;
	struct k_thread thread;
	char thread_name[24];
};

#define NODE_NAME(node) DT_PROP_OR(node, label, DT_NODE_FULL_NAME(node))
#define JTAG_GPIO(node, prop) GPIO_DT_SPEC_GET(DT_PHANDLE(node, prop), gpios)
#define JTAG_GPIO_OR(node, prop)						\
	COND_CODE_1(DT_NODE_HAS_PROP(node, prop), (JTAG_GPIO(node, prop)), ({ 0 }))
#define JTAG_SELECT(node, prop, idx)						\
	GPIO_DT_SPEC_GET(DT_PHANDLE_BY_IDX(node, prop, idx), gpios),
#define JTAG_SELECTS(node)							\
	COND_CODE_1(DT_NODE_HAS_PROP(node, select),				\
		    (DT_FOREACH_PROP_ELEM(node, select, JTAG_SELECT)), ())

/* wallabmc,jtag nodes */
#define JTAG_DEFINE(node)							\
	{									\
		.name = NODE_NAME(node),					\
		.port = DT_PROP(node, port),					\
		.tck = JTAG_GPIO(node, tck),					\
		.tms = JTAG_GPIO(node, tms),					\
		.tdi = JTAG_GPIO(node, tdi),					\
		.tdo = JTAG_GPIO(node, tdo),					\
		.trst = JTAG_GPIO_OR(node, trst),				\
		.select = { JTAG_SELECTS(node) },				\
		.nselect = DT_PROP_LEN_OR(node, select, 0),			\
		.select_mcu = DT_PROP_OR(node, select_mcu, 0),			\
		.select_idle = DT_PROP_OR(node, select_idle, -1),		\
	},

#define JTAG_CHECK(node)							\
	BUILD_ASSERT(DT_PROP_LEN_OR(node, select, 0) <= JTAG_MAX_SELECT,	\
		     "too many select lines");					\
	BUILD_ASSERT(!DT_NODE_HAS_PROP(node, select) ||				\
		     DT_NODE_HAS_PROP(node, select_mcu),			\
		     "select needs select-mcu");

DT_FOREACH_STATUS_OKAY(wallabmc_jtag, JTAG_CHECK)

static struct jtag jtags[] = {
	DT_FOREACH_STATUS_OKAY(wallabmc_jtag, JTAG_DEFINE)
};

#define NJTAG DT_NUM_INST_STATUS_OKAY(wallabmc_jtag)
K_THREAD_STACK_ARRAY_DEFINE(jtag_stacks, NJTAG, CONFIG_JTAG_DAEMON_STACK_SIZE);

static inline void set_tck(struct jtag *j, int state)
{
	if (j->tck_prev != state) {
		gpio_pin_set_dt(&j->tck, state);
		j->tck_prev = state;
	}
}

static inline void set_tms(struct jtag *j, int state)
{
	if (j->tms_prev != state) {
		gpio_pin_set_dt(&j->tms, state);
		j->tms_prev = state;
	}
}

static inline void set_tdi(struct jtag *j, int state)
{
	if (j->tdi_prev != state) {
		gpio_pin_set_dt(&j->tdi, state);
		j->tdi_prev = state;
	}
}

static inline int get_tdo(struct jtag *j)
{
	return gpio_pin_get_dt(&j->tdo);
}

/*
 * Point the mux at the MCU (value = select_mcu) or back to its idle
 * source; a negative value leaves the lines as inputs.
 */
static void jtag_select(struct jtag *j, int value)
{
	for (int i = 0; i < j->nselect; i++) {
		gpio_flags_t flags = GPIO_INPUT;

		if (value >= 0) {
			flags = (value >> i) & 1 ? GPIO_OUTPUT_ACTIVE :
						   GPIO_OUTPUT_INACTIVE;
		}
		gpio_pin_configure_dt(&j->select[i], flags);
	}
}

/* Make the JTAG pins high impedance */
static void jtag_pins_disable(struct jtag *j)
{
	gpio_pin_configure_dt(&j->tck, GPIO_INPUT);
	gpio_pin_configure_dt(&j->tms, GPIO_INPUT);
	gpio_pin_configure_dt(&j->tdi, GPIO_INPUT);
	gpio_pin_configure_dt(&j->tdo, GPIO_INPUT);
	if (j->trst.port != NULL) {
		gpio_pin_configure_dt(&j->trst, GPIO_INPUT);
	}
}

/* Drive TCK and TDI low, TMS high and release TRST */
static void jtag_pins_enable(struct jtag *j)
{
	gpio_pin_configure_dt(&j->tck, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&j->tms, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&j->tdi, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&j->tdo, GPIO_INPUT);
	if (j->trst.port != NULL) {
		gpio_pin_configure_dt(&j->trst, GPIO_OUTPUT_INACTIVE);
	}

	/* Reset cached pin states */
	j->tck_prev = -1;
	j->tms_prev = -1;
	j->tdi_prev = -1;
	j->srst_warned = false;
	j->swd_warned = false;
}

static bool jtag_pins_ready(struct jtag *j)
{
	bool ready = device_is_ready(j->tck.port) &&
		     device_is_ready(j->tms.port) &&
		     device_is_ready(j->tdi.port) &&
		     device_is_ready(j->tdo.port) &&
		     (j->trst.port == NULL || device_is_ready(j->trst.port));

	for (int i = 0; i < j->nselect; i++) {
		ready = ready && device_is_ready(j->select[i].port);
	}

	return ready;
}

static int jtag_send_all(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		int rc = send(fd, buf, len, 0);

		if (rc <= 0) {
			return -1;
		}
		buf += rc;
		len -= rc;
	}

	return 0;
}

/*
 * Serve one client. OpenOCD writes commands in bursts and reads the
 * replies to its 'R' commands afterwards, so replies are collected and
 * sent once per received chunk. Every command is one byte and the only
 * bytes ever sent back are the '0' or '1' answers to 'R' and the '0'
 * that refuses an SWD read.
 */
static void handle_client(struct jtag *j, int client_fd)
{
	int opt = 1;
	int keep_idle = 30;
	int keep_intvl = 5;
	int keep_cnt = 3;

	/* Improve performance by enabling TCP_NODELAY (ie disabling Nagle) */
	if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
		LOG_WRN("%s: failed to set TCP_NODELAY: %d", j->name, errno);
	}

	/*
	 * A peer that vanishes without closing would otherwise hold the
	 * chain and the mux for ever: probe it after 30 s of silence and
	 * give up after three unanswered probes.
	 */
	if (setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) < 0 ||
	    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle)) < 0 ||
	    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keep_intvl, sizeof(keep_intvl)) < 0 ||
	    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &keep_cnt, sizeof(keep_cnt)) < 0) {
		LOG_WRN("%s: failed to set TCP keepalive: %d", j->name, errno);
	}

	LOG_INF("%s: client connected", j->name);

	jtag_pins_enable(j);
	jtag_select(j, j->select_mcu);

	bool finished = false;
	while (!finished) {
		unsigned char cmds[JTAG_CMD_BUF];
		char replies[JTAG_CMD_BUF];
		size_t nreplies = 0;
		int rc;

		rc = recv(client_fd, cmds, sizeof(cmds), 0);
		if (rc <= 0) {
			if (rc == 0) {
				LOG_INF("%s: client disconnected", j->name);
			} else {
				LOG_WRN("%s: client recv() error: %d", j->name, errno);
			}
			break;
		}

		for (int n = 0; n < rc && !finished; n++) {
			unsigned char cmd = cmds[n];

			switch (cmd) {
			/* Blink, ignore */
			case 'b':
			case 'B':
				break;

			/* Write: bit 2 TCK, bit 1 TMS, bit 0 TDI */
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7': {
				/*
				 * We may be able to remove the lock, or
				 * perhaps change our priority to K_PRIO_COOP()
				 */
				unsigned int key = irq_lock();

				set_tms(j, cmd & 0x02);
				set_tdi(j, cmd & 0x01);
				/* Write tck last */
				set_tck(j, cmd & 0x04);
				irq_unlock(key);
				break;
			}

			/* Read */
			case 'R':
				replies[nreplies++] = get_tdo(j) ? '1' : '0';
				break;

			/* Quit */
			case 'Q':
				LOG_INF("%s: client quit", j->name);
				finished = true;
				break;

			/* Reset: bit 1 TRST, bit 0 SRST, 1 = asserted */
			case 'r':
			case 's':
			case 't':
			case 'u': {
				int trst = (cmd - 'r') & 0x02;
				int srst = (cmd - 'r') & 0x01;

				if (j->trst.port != NULL) {
					gpio_pin_set_dt(&j->trst, trst ? 1 : 0);
				}
				if (srst && !j->srst_warned) {
					LOG_WRN("%s: SRST unsupported, use the power and reset commands",
						j->name);
					j->srst_warned = true;
				}
				break;
			}

			/* Sleep, only with "remote_bitbang use_remote_sleep on" */
			case 'Z':
			case 'D': /* pre-release spelling of 'Z' */
				k_busy_wait(1000);
				break;
			case 'z':
				k_busy_wait(1);
				break;

			/* SWD: answer its read so OpenOCD fails instead of hanging */
			case 'c':
				replies[nreplies++] = '0';
				__fallthrough;
			case 'O':
			case 'o':
			case 'd':
			case 'e':
			case 'f':
			case 'g':
				if (!j->swd_warned) {
					LOG_WRN("%s: SWD unsupported", j->name);
					j->swd_warned = true;
				}
				break;

			default:
				LOG_WRN("%s: unknown command: 0x%02x", j->name, cmd);
				break; /* Unknown command, don't disconnect */
			}
		}

		if (nreplies > 0 && jtag_send_all(client_fd, replies, nreplies) < 0) {
			LOG_WRN("%s: client send() error: %d", j->name, errno);
			finished = true;
		}
	}

	jtag_select(j, j->select_idle);
	jtag_pins_disable(j);
}

static void jtag_daemon_thread(void *a, void *b, void *c)
{
	struct jtag *j = a;
	int server_fd;
	struct sockaddr_in server_addr;

	/* Pins float and the mux idles until someone connects */
	jtag_pins_disable(j);
	jtag_select(j, j->select_idle);

	server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server_fd < 0) {
		LOG_ERR("%s: socket() failed: %d", j->name, errno);
		return;
	}

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(j->port);
	server_addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		LOG_ERR("%s: bind() failed: %d", j->name, errno);
		close(server_fd);
		return;
	}

	if (listen(server_fd, 1) < 0) {
		LOG_ERR("%s: listen() failed: %d", j->name, errno);
		close(server_fd);
		return;
	}

	LOG_INF("%s: JTAG daemon listening on port %u", j->name, j->port);

	while (1) {
		struct sockaddr_in client_addr;
		socklen_t client_addr_len = sizeof(client_addr);
		int client_fd;

		client_fd = accept(server_fd, (struct sockaddr *)&client_addr,
				   &client_addr_len);
		if (client_fd < 0) {
			LOG_ERR("%s: accept() failed: %d", j->name, errno);
			continue;
		}

		handle_client(j, client_fd);

		close(client_fd);
	}
}

int jtag_init(void)
{
	for (size_t i = 0; i < NJTAG; i++) {
		struct jtag *j = &jtags[i];

		if (!jtag_pins_ready(j)) {
			LOG_ERR("%s: JTAG GPIO devices not ready, chain not served", j->name);
			continue;
		}

		snprintf(j->thread_name, sizeof(j->thread_name), "jtag %s", j->name);
		k_thread_create(&j->thread, jtag_stacks[i],
				K_THREAD_STACK_SIZEOF(jtag_stacks[i]),
				jtag_daemon_thread, j, NULL, NULL,
				CONFIG_JTAG_DAEMON_PRIORITY, 0, K_NO_WAIT);
		k_thread_name_set(&j->thread, j->thread_name);
	}

	return 0;
}
