/*
 * SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include "workq.h"

static K_KERNEL_STACK_DEFINE(blocking_work_q_stack, CONFIG_APP_BLOCKING_WORKQUEUE_STACK_SIZE);

struct k_work_q blocking_work_q;

int blocking_work_q_init(void)
{
	static const struct k_work_queue_config cfg = {
		.name = "blocking_workq",
	};

	k_work_queue_init(&blocking_work_q);
	k_work_queue_start(&blocking_work_q, blocking_work_q_stack,
			   K_KERNEL_STACK_SIZEOF(blocking_work_q_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, &cfg);

	return 0;
}
