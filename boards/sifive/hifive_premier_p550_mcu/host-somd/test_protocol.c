// SPDX-License-Identifier: Apache-2.0
/*
 * Unit tests for the HFP protocol and somd command handling.
 * Uses a minimal assert-based harness — no external dependencies.
 *
 * Build: make test
 * Run:   ./test_protocol
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "hfp_protocol.h"

static int tests_run;
static int tests_passed;

#define TEST(name) do { \
	tests_run++; \
	printf("  %-50s ", #name); \
	name(); \
	tests_passed++; \
	printf("OK\n"); \
} while (0)

/* --- Protocol tests --- */

static void test_frame_size(void)
{
	assert(sizeof(struct hfp_message) == HFP_FRAME_SIZE);
	assert(HFP_FRAME_SIZE == 267);
}

static void test_checksum_empty_data(void)
{
	struct hfp_message msg = {0};

	msg.msg_type = HFP_MSG_REPLY;
	msg.cmd_type = HFP_CMD_BOARD_STATUS;
	msg.data_len = 0;

	uint8_t cs = hfp_checksum(&msg);
	assert(cs == (0x02 ^ 0x06 ^ 0x00));
}

static void test_checksum_with_data(void)
{
	struct hfp_message msg = {0};

	msg.msg_type = HFP_MSG_REQUEST;
	msg.cmd_type = HFP_CMD_PVT_INFO;
	msg.data_len = 3;
	msg.data[0] = 0xAA;
	msg.data[1] = 0xBB;
	msg.data[2] = 0xCC;

	uint8_t expected = 0x01 ^ 0x05 ^ 0x03 ^ 0xAA ^ 0xBB ^ 0xCC;
	assert(hfp_checksum(&msg) == expected);
}

static void test_checksum_excludes_cmd_result(void)
{
	struct hfp_message msg1 = {0};
	struct hfp_message msg2 = {0};

	msg1.msg_type = HFP_MSG_REPLY;
	msg1.cmd_type = HFP_CMD_BOARD_STATUS;
	msg1.cmd_result = HFP_RESULT_OK;

	msg2.msg_type = HFP_MSG_REPLY;
	msg2.cmd_type = HFP_CMD_BOARD_STATUS;
	msg2.cmd_result = HFP_RESULT_ERROR;

	assert(hfp_checksum(&msg1) == hfp_checksum(&msg2));
}

static void test_magic_values(void)
{
	assert(HFP_MAGIC_HEADER == 0xA55AAA55);
	assert(HFP_MAGIC_TAIL == 0xBDBABDBA);
}

static void test_command_enum_values(void)
{
	assert(HFP_CMD_POWER_OFF == 0x01);
	assert(HFP_CMD_REBOOT == 0x02);
	assert(HFP_CMD_BOARD_INFO == 0x03);
	assert(HFP_CMD_PVT_INFO == 0x05);
	assert(HFP_CMD_BOARD_STATUS == 0x06);
	assert(HFP_CMD_RESTART == 0x08);
}

static void test_pvt_info_struct_size(void)
{
	assert(sizeof(struct hfp_pvt_info) == 12);
}

/* --- Reply construction tests --- */

static void test_build_board_status_reply(void)
{
	struct hfp_message req = {
		.header = HFP_MAGIC_HEADER,
		.task_id = 0x12345678,
		.msg_type = HFP_MSG_REQUEST,
		.cmd_type = HFP_CMD_BOARD_STATUS,
		.tail = HFP_MAGIC_TAIL,
	};

	struct hfp_message reply;

	memset(&reply, 0, sizeof(reply));
	reply.header = HFP_MAGIC_HEADER;
	reply.task_id = req.task_id;
	reply.msg_type = HFP_MSG_REPLY;
	reply.cmd_type = req.cmd_type;
	reply.cmd_result = HFP_RESULT_OK;
	reply.tail = HFP_MAGIC_TAIL;
	reply.checksum = hfp_checksum(&reply);

	assert(reply.header == HFP_MAGIC_HEADER);
	assert(reply.task_id == 0x12345678);
	assert(reply.msg_type == HFP_MSG_REPLY);
	assert(reply.cmd_type == HFP_CMD_BOARD_STATUS);
	assert(reply.cmd_result == HFP_RESULT_OK);
	assert(reply.data_len == 0);
	assert(reply.tail == HFP_MAGIC_TAIL);
	assert(reply.checksum == hfp_checksum(&reply));
}

static void test_build_power_off_reply(void)
{
	struct hfp_message req = {
		.header = HFP_MAGIC_HEADER,
		.task_id = 0xAABBCCDD,
		.msg_type = HFP_MSG_REQUEST,
		.cmd_type = HFP_CMD_POWER_OFF,
		.tail = HFP_MAGIC_TAIL,
	};

	struct hfp_message reply;

	memset(&reply, 0, sizeof(reply));
	reply.header = HFP_MAGIC_HEADER;
	reply.task_id = req.task_id;
	reply.msg_type = HFP_MSG_REPLY;
	reply.cmd_type = req.cmd_type;
	reply.cmd_result = HFP_RESULT_OK;
	reply.tail = HFP_MAGIC_TAIL;
	reply.checksum = hfp_checksum(&reply);

	assert(reply.task_id == req.task_id);
	assert(reply.cmd_type == HFP_CMD_POWER_OFF);
	assert(reply.cmd_result == HFP_RESULT_OK);
}

static void test_build_restart_reply(void)
{
	struct hfp_message req = {
		.header = HFP_MAGIC_HEADER,
		.task_id = 0x11223344,
		.msg_type = HFP_MSG_REQUEST,
		.cmd_type = HFP_CMD_RESTART,
		.tail = HFP_MAGIC_TAIL,
	};

	struct hfp_message reply;

	memset(&reply, 0, sizeof(reply));
	reply.header = HFP_MAGIC_HEADER;
	reply.task_id = req.task_id;
	reply.msg_type = HFP_MSG_REPLY;
	reply.cmd_type = req.cmd_type;
	reply.cmd_result = HFP_RESULT_OK;
	reply.tail = HFP_MAGIC_TAIL;
	reply.checksum = hfp_checksum(&reply);

	assert(reply.task_id == req.task_id);
	assert(reply.cmd_type == HFP_CMD_RESTART);
	assert(reply.cmd_result == HFP_RESULT_OK);
}

int main(void)
{
	printf("somd protocol tests:\n");

	TEST(test_frame_size);
	TEST(test_checksum_empty_data);
	TEST(test_checksum_with_data);
	TEST(test_checksum_excludes_cmd_result);
	TEST(test_magic_values);
	TEST(test_command_enum_values);
	TEST(test_pvt_info_struct_size);
	TEST(test_build_board_status_reply);
	TEST(test_build_power_off_reply);
	TEST(test_build_restart_reply);

	printf("\n%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
