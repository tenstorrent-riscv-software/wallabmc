/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(console_logger, LOG_LEVEL_INF);

#include <zephyr/spinlock.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <errno.h>
#include <string.h>

#include "synch.h"

#define UART_NODE DT_ALIAS(host_console_uart)
static const struct device *uart_dev = DEVICE_DT_GET(UART_NODE);

BUILD_ASSERT(DT_NODE_EXISTS(UART_NODE), "host-console-uart node missing");

/* UART mux select GPIO from device tree */
#define UARTMUXSEL_NODE DT_ALIAS(host_console_uart_muxsel)
#if DT_NODE_EXISTS(UARTMUXSEL_NODE)
static const struct gpio_dt_spec host_console_uart_muxsel_gpio = GPIO_DT_SPEC_GET(UARTMUXSEL_NODE, gpios);
#endif

/*
 * Max bytes to drain from the UART RX FIFO per IRQ. Larger keeps each
 * IRQ self-contained at higher line rates; smaller bounds the time
 * spent in the handler. 64 is comfortable at 115200 (~5.6 ms of bytes).
 */
#define UART_RX_DRAIN_CHUNK 64

struct console_log {
	const struct device *uart;
	uint64_t received;
	int size;
	struct k_spinlock rx_lock;

	/* TX is serialized by tx_mutex; tx_done is given by the IRQ when
	 * the pending buffer has been fully pushed to the FIFO.
	 */
	struct k_mutex tx_mutex;
	struct k_sem tx_done;
	const uint8_t *tx_buf;
	size_t tx_len;
	size_t tx_pos;

	uint8_t *log_buffer;
};

static struct console_log host_console_log;

static uint8_t log_buffer[CONFIG_APP_CONSOLE_LOG_SIZE];

static void uart_isr_cb(const struct device *dev, void *user_data)
{
	struct console_log *log = user_data;

	while (uart_irq_update(dev) > 0 && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t tmp[UART_RX_DRAIN_CHUNK];
			int n = uart_fifo_read(dev, tmp, sizeof(tmp));

			if (n > 0) {
				k_spinlock_key_t key = k_spin_lock(&log->rx_lock);
				size_t off = log->received % log->size;
				size_t first = MIN((size_t)n, (size_t)log->size - off);

				memcpy(log->log_buffer + off, tmp, first);
				if (first < (size_t)n)
					memcpy(log->log_buffer, tmp + first, (size_t)n - first);
				log->received += (size_t)n;
				k_spin_unlock(&log->rx_lock, key);

				k_event_post(&events, EVENT_CONSOLE_LOG_DATA);
			}
		}

		if (uart_irq_tx_ready(dev)) {
			if (log->tx_pos < log->tx_len) {
				int n = uart_fifo_fill(dev, log->tx_buf + log->tx_pos,
						       log->tx_len - log->tx_pos);
				if (n > 0)
					log->tx_pos += n;
			}
			if (log->tx_pos >= log->tx_len) {
				uart_irq_tx_disable(dev);
				k_sem_give(&log->tx_done);
			}
		}
	}
}

static ssize_t console_log_read(struct console_log *log, uint8_t *buf, size_t size, uint64_t *ppos)
{
	uint64_t pos = *ppos;
	uint64_t start;
	int off, len;
	k_spinlock_key_t key;
	ssize_t ret = 0;
	size_t copied = 0;

	key = k_spin_lock(&log->rx_lock);
	if (pos > log->received) {
		ret = -EINVAL;
		goto out;
	}

	if (log->received < log->size) {
		start = 0;
	} else {
		start = log->received - log->size;
	}

	if (start > pos)
		pos = start; /* lost characters, advance pos */

	while (copied < size) {
		off = pos % log->size;
		len = MIN(log->received - pos, log->size - off);
		len = MIN(len, size - copied);
		if (len == 0)
			break;
		memcpy(buf + copied, log->log_buffer + off, len);
		pos += len;
		copied += len;
	}

out:
	k_spin_unlock(&log->rx_lock, key);

	*ppos = pos;

	return copied ? copied : ret;
}

static ssize_t console_log_write(struct console_log *log, const uint8_t *buf, size_t size)
{
	k_mutex_lock(&log->tx_mutex, K_FOREVER);

	log->tx_buf = buf;
	log->tx_len = size;
	log->tx_pos = 0;
	k_sem_reset(&log->tx_done);

	uart_irq_tx_enable(log->uart);
	k_sem_take(&log->tx_done, K_FOREVER);

	log->tx_buf = NULL;
	log->tx_len = 0;
	log->tx_pos = 0;

	k_mutex_unlock(&log->tx_mutex);
	return size;
}

ssize_t host_console_read(uint8_t *buf, size_t size, uint64_t *ppos)
{
	struct console_log *log = &host_console_log;
	return console_log_read(log, buf, size, ppos);
}

int host_console_seek_end(uint64_t *ppos)
{
	struct console_log *log = &host_console_log;
	k_spinlock_key_t key;

	key = k_spin_lock(&log->rx_lock);
	*ppos = log->received;
	k_spin_unlock(&log->rx_lock, key);

	return 0;
}

ssize_t host_console_write(const uint8_t *buf, size_t size)
{
	struct console_log *log = &host_console_log;

	return console_log_write(log, buf, size);
}

static const struct uart_config uart_cfg = {
	.baudrate = 115200,
	.parity = UART_CFG_PARITY_NONE,
	.stop_bits = UART_CFG_STOP_BITS_1,
	.data_bits = UART_CFG_DATA_BITS_8,
	.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
};

int console_logger_init(void)
{
	struct console_log *log = &host_console_log;
	int ret;

	LOG_INF("Starting host console logger");

	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -1;
	}

	memset(log, 0, sizeof(struct console_log));
	log->uart = uart_dev;
	log->size = sizeof(log_buffer);
	k_mutex_init(&log->tx_mutex);
	k_sem_init(&log->tx_done, 0, 1);
	log->log_buffer = log_buffer;

#if DT_NODE_EXISTS(UARTMUXSEL_NODE)
	/* Initialize host_console_uart_muxsel GPIO */
	if (!gpio_is_ready_dt(&host_console_uart_muxsel_gpio)) {
		LOG_ERR("host_console_uart_muxsel GPIO device not ready");
		return -1;
	}

	ret = gpio_pin_configure_dt(&host_console_uart_muxsel_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure host_console_uart_muxsel GPIO: %d", ret);
		return ret;
	}

	gpio_pin_set_dt(&host_console_uart_muxsel_gpio, 1);
	LOG_INF("host_console_uart_muxsel GPIO set");
#endif

	ret = uart_configure(uart_dev, &uart_cfg);
	if (ret < 0) {
		LOG_ERR("UART configure failed: %d", ret);
		return -1;
	}

	ret = uart_irq_callback_user_data_set(uart_dev, uart_isr_cb, log);
	if (ret < 0) {
		LOG_ERR("Failed to set UART IRQ callback: %d", ret);
		return ret;
	}

	uart_irq_rx_enable(uart_dev);

	LOG_INF("UART IRQ configured for console logger");

	return 0;
}
