/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __CONSOLE_LOGGER_H__
#define __CONSOLE_LOGGER_H__

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/spinlock.h>
#include <sys/types.h>

#ifdef CONFIG_APP_CONSOLE_LOGGER

#define NCONSOLE DT_NUM_INST_STATUS_OKAY(wallabmc_console)

/* Bits of struct console::events; each consumer has its own data bit */
#define CONSOLE_EVENT_DATA_TCP		BIT(0)
#define CONSOLE_EVENT_DATA_WS		BIT(1)
#define CONSOLE_EVENT_TCP_CLIENT	BIT(2)
#define CONSOLE_EVENT_WS_CLIENT		BIT(3)

/* One wallabmc,console devicetree node */
struct console {
	const char *name;
	const char *endpoint;
	uint16_t ord;		/* devicetree ordinal */
	uint16_t port;
	const struct device *uart;
	uint32_t baud;
	struct gpio_dt_spec mux;	/* .port NULL without mux-select */
	bool ready;

	/* Ring of everything received, written by the UART ISR */
	uint8_t *buf;
	size_t size;
	uint64_t received;
	struct k_spinlock rx_lock;

	/*
	 * TX is serialised by tx_mutex; tx_done is given by the ISR once
	 * the pending buffer has been pushed to the FIFO.
	 */
	struct k_mutex tx_mutex;
	struct k_sem tx_done;
	const uint8_t *tx_buf;
	size_t tx_len;
	size_t tx_pos;

	struct k_event events;
	struct k_work_delayable rx_work;
};

extern struct console consoles[NCONSOLE];

int console_logger_init(void);
ssize_t console_read(struct console *c, uint8_t *buf, size_t size, uint64_t *ppos);
void console_seek_end(struct console *c, uint64_t *ppos);
ssize_t console_write(struct console *c, const uint8_t *buf, size_t size);
#else
static inline int console_logger_init(void) { return 0; }
#endif

#endif
