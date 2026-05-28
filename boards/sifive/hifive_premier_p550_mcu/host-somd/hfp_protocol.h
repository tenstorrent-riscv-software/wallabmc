/* SPDX-License-Identifier: Apache-2.0 */
/*
 * HiFive Premier P550 BMC-SoC UART protocol definitions.
 *
 * Matches the framed message protocol used between the STM32 BMC (MCU UART4)
 * and the EIC7700X SoC (SoC UART2) at 115200 8N1.
 *
 * References:
 *   - WallaBMC src/som_protocol.h (BMC side)
 *   - OpenSBI platform/generic/include/eswin/hfp.h (SoC side)
 *   - ESWIN vendor firmware hf_common.h (original)
 */

#ifndef HFP_PROTOCOL_H
#define HFP_PROTOCOL_H

#include <stdint.h>

#define HFP_MAGIC_HEADER	0xA55AAA55
#define HFP_MAGIC_TAIL		0xBDBABDBA
#define HFP_DATA_MAX		250
#define HFP_FRAME_SIZE		267

enum hfp_msg_type {
	HFP_MSG_REQUEST = 0x01,
	HFP_MSG_REPLY   = 0x02,
	HFP_MSG_NOTIFY  = 0x03,
};

enum hfp_cmd_type {
	HFP_CMD_POWER_OFF    = 0x01,
	HFP_CMD_REBOOT       = 0x02,
	HFP_CMD_BOARD_INFO   = 0x03,
	HFP_CMD_CONTROL_LED  = 0x04,
	HFP_CMD_PVT_INFO     = 0x05,
	HFP_CMD_BOARD_STATUS = 0x06,
	HFP_CMD_POWER_INFO   = 0x07,
	HFP_CMD_RESTART      = 0x08,
};

enum hfp_result {
	HFP_RESULT_OK          = 0x00,
	HFP_RESULT_ERROR       = 0x01,
	HFP_RESULT_INVALID     = 0x02,
	HFP_RESULT_UNSUPPORTED = 0x03,
};

struct hfp_message {
	uint32_t header;
	uint32_t task_id;
	uint8_t  msg_type;
	uint8_t  cmd_type;
	uint8_t  cmd_result;
	uint8_t  data_len;
	uint8_t  data[HFP_DATA_MAX];
	uint8_t  checksum;
	uint32_t tail;
} __attribute__((packed));

_Static_assert(sizeof(struct hfp_message) == HFP_FRAME_SIZE,
	       "hfp_message must be 267 bytes");

struct hfp_pvt_info {
	int32_t cpu_temp;
	int32_t npu_temp;
	int32_t fan_speed;
} __attribute__((packed));

static inline uint8_t hfp_checksum(const struct hfp_message *msg)
{
	uint8_t cs = 0;

	cs ^= msg->msg_type;
	cs ^= msg->cmd_type;
	cs ^= msg->data_len;
	for (int i = 0; i < msg->data_len; i++)
		cs ^= msg->data[i];
	return cs;
}

#endif /* HFP_PROTOCOL_H */
