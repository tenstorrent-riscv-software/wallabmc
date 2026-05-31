// SPDX-FileCopyrightText: © 2026 Red Hat, LLC
// SPDX-License-Identifier: Apache-2.0
/*
 * somd - SOM daemon for HiFive Premier P550
 *
 * Host-side responder for the BMC UART protocol. Listens on SoC UART2
 * (typically /dev/ttyS2), receives framed requests from the BMC, and
 * replies with board status, thermal data, etc.
 *
 * Usage: somd [-d /dev/ttyS2] [-f]
 *   -d  Serial device path (default: /dev/ttyS2)
 *   -f  Run in foreground (default: background via systemd)
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>

#include "hfp_protocol.h"

static volatile sig_atomic_t running = 1;
static int use_syslog = 1;
static int verbose;

static void log_msg(int priority, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (use_syslog) {
		vsyslog(priority, fmt, ap);
	} else {
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	}
	va_end(ap);
}

static void sig_handler(int sig)
{
	(void)sig;
	running = 0;
}

static int serial_open(const char *path)
{
	struct termios tty;
	int fd;

	fd = open(path, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		log_msg(LOG_ERR, "open %s: %s", path, strerror(errno));
		return -1;
	}

	if (tcgetattr(fd, &tty) < 0) {
		log_msg(LOG_ERR, "tcgetattr: %s", strerror(errno));
		close(fd);
		return -1;
	}

	cfmakeraw(&tty);
	cfsetispeed(&tty, B115200);
	cfsetospeed(&tty, B115200);

	tty.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
	tty.c_cflag |= CS8 | CLOCAL | CREAD;
	tty.c_cc[VMIN] = 1;
	tty.c_cc[VTIME] = 0;

	if (tcsetattr(fd, TCSANOW, &tty) < 0) {
		log_msg(LOG_ERR, "tcsetattr: %s", strerror(errno));
		close(fd);
		return -1;
	}

	tcflush(fd, TCIOFLUSH);
	return fd;
}

static int read_exact(int fd, void *buf, size_t len)
{
	size_t pos = 0;
	uint8_t *p = buf;

	while (pos < len) {
		ssize_t n = read(fd, p + pos, len - pos);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		pos += n;
	}
	return 0;
}

static int write_exact(int fd, const void *buf, size_t len)
{
	size_t pos = 0;
	const uint8_t *p = buf;

	while (pos < len) {
		ssize_t n = write(fd, p + pos, len - pos);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		pos += n;
	}
	return 0;
}

/*
 * Scan the byte stream for the 4-byte magic header.
 * Returns 0 on success with the header already consumed, -1 on error.
 */
static int sync_to_header(int fd)
{
	const uint8_t hdr[] = {
		(HFP_MAGIC_HEADER >>  0) & 0xFF,
		(HFP_MAGIC_HEADER >>  8) & 0xFF,
		(HFP_MAGIC_HEADER >> 16) & 0xFF,
		(HFP_MAGIC_HEADER >> 24) & 0xFF,
	};
	int match = 0;
	uint8_t byte;

	while (running) {
		ssize_t n = read(fd, &byte, 1);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;

		if (verbose > 1)
			log_msg(LOG_DEBUG, "rx byte: 0x%02x (match=%d)", byte, match);

		if (byte == hdr[match]) {
			match++;
			if (match == 4) {
				if (verbose)
					log_msg(LOG_INFO, "header sync");
				return 0;
			}
		} else {
			match = (byte == hdr[0]) ? 1 : 0;
		}
	}
	return -1;
}

static int read_thermal(const char *path)
{
	FILE *f;
	int val = -1;

	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%d", &val) != 1)
		val = -1;
	fclose(f);
	return val;
}

/*
 * Try common sysfs thermal zone paths for the EIC7700X.
 * Returns temperature in millidegrees Celsius, or -1.
 */
static int read_cpu_temp(void)
{
	int val;

	val = read_thermal("/sys/class/thermal/thermal_zone0/temp");
	if (val >= 0)
		return val;
	return -1;
}

static int read_npu_temp(void)
{
	int val;

	val = read_thermal("/sys/class/thermal/thermal_zone1/temp");
	if (val >= 0)
		return val;
	return -1;
}

static void handle_pvt_info(struct hfp_message *reply)
{
	struct hfp_pvt_info pvt = {
		.cpu_temp  = read_cpu_temp(),
		.npu_temp  = read_npu_temp(),
		.fan_speed = -1,
	};

	reply->data_len = sizeof(pvt);
	memcpy(reply->data, &pvt, sizeof(pvt));
}

static void send_reply(int fd, const struct hfp_message *req,
		       uint8_t result, const uint8_t *data, uint8_t data_len)
{
	struct hfp_message reply;

	memset(&reply, 0, sizeof(reply));
	reply.header = HFP_MAGIC_HEADER;
	reply.task_id = req->task_id;
	reply.msg_type = HFP_MSG_REPLY;
	reply.cmd_type = req->cmd_type;
	reply.cmd_result = result;
	reply.tail = HFP_MAGIC_TAIL;

	if (data && data_len > 0) {
		reply.data_len = data_len;
		memcpy(reply.data, data, data_len);
	}

	reply.checksum = hfp_checksum(&reply);

	if (write_exact(fd, &reply, sizeof(reply)) < 0)
		log_msg(LOG_ERR, "write reply: %s", strerror(errno));
}

static void send_notify(int fd, uint8_t cmd)
{
	struct hfp_message msg;

	memset(&msg, 0, sizeof(msg));
	msg.header = HFP_MAGIC_HEADER;
	msg.msg_type = HFP_MSG_NOTIFY;
	msg.cmd_type = cmd;
	msg.tail = HFP_MAGIC_TAIL;
	msg.checksum = hfp_checksum(&msg);

	write_exact(fd, &msg, sizeof(msg));
	tcdrain(fd);
}

static void handle_request(int fd, const struct hfp_message *req)
{
	struct hfp_message reply;

	switch (req->cmd_type) {
	case HFP_CMD_BOARD_STATUS:
		send_reply(fd, req, HFP_RESULT_OK, NULL, 0);
		break;

	case HFP_CMD_PVT_INFO:
		memset(&reply, 0, sizeof(reply));
		reply.header = HFP_MAGIC_HEADER;
		reply.task_id = req->task_id;
		reply.msg_type = HFP_MSG_REPLY;
		reply.cmd_type = req->cmd_type;
		reply.cmd_result = HFP_RESULT_OK;
		reply.tail = HFP_MAGIC_TAIL;
		handle_pvt_info(&reply);
		reply.checksum = hfp_checksum(&reply);
		if (write_exact(fd, &reply, sizeof(reply)) < 0)
			log_msg(LOG_ERR, "write reply: %s", strerror(errno));
		break;

	case HFP_CMD_POWER_OFF:
		log_msg(LOG_INFO, "BMC requested poweroff");
		send_reply(fd, req, HFP_RESULT_OK, NULL, 0);
		tcdrain(fd);
		if (system("/sbin/poweroff") != 0)
			log_msg(LOG_ERR, "poweroff failed");
		send_notify(fd, HFP_CMD_POWER_OFF);
		running = 0;
		break;

	case HFP_CMD_RESTART:
		log_msg(LOG_INFO, "BMC requested reboot");
		send_reply(fd, req, HFP_RESULT_OK, NULL, 0);
		tcdrain(fd);
		if (system("/sbin/reboot") != 0)
			log_msg(LOG_ERR, "reboot failed");
		send_notify(fd, HFP_CMD_RESTART);
		running = 0;
		break;

	default:
		log_msg(LOG_DEBUG, "unsupported cmd 0x%02x", req->cmd_type);
		send_reply(fd, req, HFP_RESULT_UNSUPPORTED, NULL, 0);
		break;
	}
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-d device] [-f] [-v]\n", prog);
	fprintf(stderr, "  -d  Serial device (default: /dev/ttyS2)\n");
	fprintf(stderr, "  -f  Foreground mode\n");
	fprintf(stderr, "  -v  Verbose (repeat for more)\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	const char *dev = "/dev/ttyS2";
	int foreground = 0;
	int fd, opt;

	while ((opt = getopt(argc, argv, "d:fvh")) != -1) {
		switch (opt) {
		case 'd':
			dev = optarg;
			break;
		case 'f':
			foreground = 1;
			break;
		case 'v':
			verbose++;
			break;
		default:
			usage(argv[0]);
		}
	}

	if (foreground)
		use_syslog = 0;
	else
		openlog("somd", LOG_PID, LOG_DAEMON);

	signal(SIGTERM, sig_handler);
	signal(SIGINT, sig_handler);

	fd = serial_open(dev);
	if (fd < 0)
		return 1;

	log_msg(LOG_INFO, "somd started on %s", dev);

	while (running) {
		struct hfp_message msg;

		if (sync_to_header(fd) < 0)
			break;

		msg.header = HFP_MAGIC_HEADER;
		if (read_exact(fd, (uint8_t *)&msg + 4, sizeof(msg) - 4) < 0) {
			if (running)
				log_msg(LOG_ERR, "read: %s", strerror(errno));
			break;
		}

		if (verbose)
			log_msg(LOG_INFO, "frame: type=0x%02x cmd=0x%02x result=0x%02x len=%d tail=0x%08x task=0x%08x",
				msg.msg_type, msg.cmd_type, msg.cmd_result,
				msg.data_len, msg.tail, msg.task_id);

		if (msg.tail != HFP_MAGIC_TAIL) {
			log_msg(LOG_WARNING, "bad tail 0x%08x", msg.tail);
			continue;
		}

		if (hfp_checksum(&msg) != msg.checksum) {
			log_msg(LOG_WARNING, "checksum mismatch: got 0x%02x expected 0x%02x",
				msg.checksum, hfp_checksum(&msg));
			continue;
		}

		if (msg.msg_type == HFP_MSG_REQUEST) {
			if (verbose)
				log_msg(LOG_INFO, "replying to cmd 0x%02x", msg.cmd_type);
			handle_request(fd, &msg);
		}
	}

	log_msg(LOG_INFO, "somd stopping");
	close(fd);

	if (!foreground)
		closelog();

	return 0;
}
