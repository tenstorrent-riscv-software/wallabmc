/*
 * SPDX-FileCopyrightText: © 2026 Red Hat, LLC
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __SOM_PROTOCOL_H__
#define __SOM_PROTOCOL_H__

#include <stdint.h>
#include <stdbool.h>

#define SOM_FRAME_HEADER	0xA55AAA55
#define SOM_FRAME_TAIL		0xBDBABDBA
#define SOM_FRAME_DATA_MAX	250

enum som_msg_type {
	SOM_MSG_REQUEST  = 0x01,
	SOM_MSG_REPLY    = 0x02,
	SOM_MSG_NOTIFY   = 0x03,
};

enum som_cmd_type {
	SOM_CMD_POWER_OFF    = 0x01,
	SOM_CMD_REBOOT       = 0x02,
	SOM_CMD_BOARD_INFO   = 0x03,
	SOM_CMD_CONTROL_LED  = 0x04,
	SOM_CMD_PVT_INFO     = 0x05,
	SOM_CMD_BOARD_STATUS = 0x06,
	SOM_CMD_POWER_INFO   = 0x07,
	SOM_CMD_RESTART      = 0x08,
};

struct som_message {
	uint32_t header;
	uint32_t task_id;
	uint8_t  msg_type;
	uint8_t  cmd_type;
	uint8_t  cmd_result;
	uint8_t  data_len;
	uint8_t  data[SOM_FRAME_DATA_MAX];
	uint8_t  checksum;
	uint32_t tail;
} __packed;

struct som_pvt_info {
	int32_t cpu_temp;
	int32_t npu_temp;
	int32_t fan_speed;
} __packed;

struct som_power_info {
	uint32_t consumption;
	uint32_t current;
	uint32_t voltage;
} __packed;

#ifdef CONFIG_SOM_PROTOCOL
int som_protocol_init(void);
int som_cmd(uint8_t cmd, void *data, size_t data_len, uint32_t timeout);
int som_get_pvt_info(struct som_pvt_info *info);
bool som_is_alive(void);
void som_set_alive(bool alive);
typedef void (*som_notify_cb_t)(uint8_t cmd_type);
void som_set_notify_callback(som_notify_cb_t cb);
#else
static inline int som_protocol_init(void) { return 0; }
static inline int som_cmd(uint8_t c, void *d, size_t l, uint32_t t) { return -ENOTSUP; }
static inline int som_get_pvt_info(struct som_pvt_info *i) { return -ENOTSUP; }
static inline bool som_is_alive(void) { return false; }
static inline void som_set_alive(bool alive) { }
static inline void som_set_notify_callback(void (*cb)(uint8_t)) { }
#endif

#endif
