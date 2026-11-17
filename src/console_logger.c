/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(console_logger, LOG_LEVEL_INF);

#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <errno.h>
#include <string.h>

#include "console_logger.h"

#define CONSOLE_MUX(node)							\
	COND_CODE_1(DT_NODE_HAS_PROP(node, mux_select),				\
		    (GPIO_DT_SPEC_GET(DT_PHANDLE(node, mux_select), gpios)),	\
		    ({ 0 }))

#define CONSOLE_DEFINE(node) {							\
	.name = DT_PROP(node, label),						\
	.endpoint = DT_PROP(node, endpoint),					\
	.ord = DT_DEP_ORD(node),						\
	.port = DT_PROP(node, port),						\
	.uart = DEVICE_DT_GET(DT_PHANDLE(node, uart)),				\
	.baud = DT_PROP_OR(DT_PHANDLE(node, uart), current_speed, 115200),	\
	.mux = CONSOLE_MUX(node),						\
},

struct console consoles[] = {
	DT_FOREACH_STATUS_OKAY(wallabmc_console, CONSOLE_DEFINE)
};

BUILD_ASSERT(ARRAY_SIZE(consoles) == NCONSOLE);

static uint8_t log_buffers[NCONSOLE][CONFIG_APP_CONSOLE_LOG_SIZE];

/* Delay notifying console readers of new data to coalesce data */
#define RX_DELAY_TIME_MS 5

static void rx_data_work_fn(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct console *c = CONTAINER_OF(dwork, struct console, rx_work);

	k_event_post(&c->events, CONSOLE_EVENT_DATA_TCP | CONSOLE_EVENT_DATA_WS);
}

static void uart_isr_cb(const struct device *dev, void *user_data)
{
	struct console *c = user_data;

	for(;;) {
		uart_irq_update(dev);
		if (!uart_irq_is_pending(dev))
			break;

		if (uart_irq_rx_ready(dev) > 0) {
			/*
			 * uart_fifo_read() must read all data from FIFO after
			 * uart_irq_rx_ready() returns true.
			 */
			for (;;) {
				k_spinlock_key_t key = k_spin_lock(&c->rx_lock);
				size_t off = c->received % c->size;
				size_t size = c->size - off;
				int nr = uart_fifo_read(dev, c->buf + off, size);
				if (nr > 0)
					c->received += (size_t)nr;
				k_spin_unlock(&c->rx_lock, key);
				if (nr < 0 || (size_t)nr < size) /* short/zero reads and -ve errors */
					break;
			}
			if (RX_DELAY_TIME_MS)
				k_work_schedule(&c->rx_work, K_MSEC(RX_DELAY_TIME_MS));
			else
				k_event_post(&c->events,
					     CONSOLE_EVENT_DATA_TCP | CONSOLE_EVENT_DATA_WS);
		}

		if (uart_irq_tx_ready(dev) > 0) {
			if (c->tx_pos < c->tx_len) {
				int nr = uart_fifo_fill(dev, c->tx_buf + c->tx_pos,
							c->tx_len - c->tx_pos);
				if (nr > 0)
					c->tx_pos += nr;
			}
			if (c->tx_pos >= c->tx_len) {
				uart_irq_tx_disable(dev);
				k_sem_give(&c->tx_done);
			}
		}
	}
}

ssize_t console_read(struct console *c, uint8_t *buf, size_t size, uint64_t *ppos)
{
	uint64_t pos = *ppos;
	uint64_t start;
	int off, len;
	k_spinlock_key_t key;
	ssize_t ret = 0;
	size_t copied = 0;

	key = k_spin_lock(&c->rx_lock);
	if (pos > c->received) {
		ret = -EINVAL;
		goto out;
	}

	if (c->received < c->size) {
		start = 0;
	} else {
		start = c->received - c->size;
	}

	if (start > pos)
		pos = start; /* lost characters, advance pos */

	while (copied < size) {
		off = pos % c->size;
		len = MIN(c->received - pos, c->size - off);
		len = MIN(len, size - copied);
		if (len == 0)
			break;
		memcpy(buf + copied, c->buf + off, len);
		pos += len;
		copied += len;
	}

out:
	k_spin_unlock(&c->rx_lock, key);

	*ppos = pos;

	return copied ? copied : ret;
}

void console_seek_end(struct console *c, uint64_t *ppos)
{
	k_spinlock_key_t key;

	key = k_spin_lock(&c->rx_lock);
	*ppos = c->received;
	k_spin_unlock(&c->rx_lock, key);
}

/*
 * Blocks until the ISR has pushed the whole buffer to the FIFO, but not
 * forever: a UART whose TX interrupt never fires must not wedge the
 * bridge that writes to it.
 */
ssize_t console_write(struct console *c, const uint8_t *buf, size_t size)
{
	ssize_t ret = size;

	if (!c->ready || size == 0)
		return 0;

	k_mutex_lock(&c->tx_mutex, K_FOREVER);

	c->tx_buf = buf;
	c->tx_len = size;
	c->tx_pos = 0;
	k_sem_reset(&c->tx_done);

	uart_irq_tx_enable(c->uart);
	if (k_sem_take(&c->tx_done, K_MSEC(200 + size * 10 * 1000 / c->baud)) < 0) {
		uart_irq_tx_disable(c->uart);
		LOG_WRN("%s console: TX timed out after %u of %u bytes", c->name,
			(unsigned int)c->tx_pos, (unsigned int)size);
		ret = -ETIMEDOUT;
	}

	c->tx_buf = NULL;
	c->tx_len = 0;
	c->tx_pos = 0;

	k_mutex_unlock(&c->tx_mutex);
	return ret;
}

/* A console that fails to start is logged and skipped; the others carry on */
int console_logger_init(void)
{
	int i;

	for (i = 0; i < NCONSOLE; i++) {
		struct console *c = &consoles[i];
		const struct uart_config cfg = {
			.baudrate = c->baud,
			.parity = UART_CFG_PARITY_NONE,
			.stop_bits = UART_CFG_STOP_BITS_1,
			.data_bits = UART_CFG_DATA_BITS_8,
			.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
		};
		int ret;

		c->buf = log_buffers[i];
		c->size = CONFIG_APP_CONSOLE_LOG_SIZE;
		k_mutex_init(&c->tx_mutex);
		k_sem_init(&c->tx_done, 0, 1);
		k_event_init(&c->events);
		k_work_init_delayable(&c->rx_work, rx_data_work_fn);

		if (!device_is_ready(c->uart)) {
			LOG_ERR("%s console: UART not ready", c->name);
			continue;
		}

		if (c->mux.port) {
			if (!gpio_is_ready_dt(&c->mux)) {
				LOG_ERR("%s console: mux select not ready", c->name);
				continue;
			}
			ret = gpio_pin_configure_dt(&c->mux, GPIO_OUTPUT_ACTIVE);
			if (ret < 0) {
				LOG_ERR("%s console: mux select failed: %d", c->name, ret);
				continue;
			}
		}

		/* A driver without runtime configuration already runs at the devicetree's speed */
		ret = uart_configure(c->uart, &cfg);
		if (ret < 0 && ret != -ENOSYS && ret != -ENOTSUP) {
			LOG_ERR("%s console: UART configure failed: %d", c->name, ret);
			continue;
		}

		ret = uart_irq_callback_user_data_set(c->uart, uart_isr_cb, c);
		if (ret < 0) {
			LOG_ERR("%s console: UART IRQ callback failed: %d", c->name, ret);
			continue;
		}

		uart_irq_rx_enable(c->uart);
		c->ready = true;
		LOG_INF("%s console on %s at %u baud", c->name, c->uart->name, c->baud);
	}

	return 0;
}
