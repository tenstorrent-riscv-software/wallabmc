/*
 * SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __WORKQ_H__
#define __WORKQ_H__

#include <zephyr/kernel.h>

#if defined(CONFIG_APP_BLOCKING_WORKQUEUE)
/*
 * Workqueue for blocking, low priority work.
 *
 * Items on the system workqueue must not block: every other item on it
 * waits behind them, including the timers Zephyr subsystems run there
 * (DHCP, DNS resolver query timeouts, ...). Work that may block for a
 * long time, such as a DNS lookup or a network request, goes on this
 * queue instead, with k_work_submit_to_queue() or
 * k_work_schedule_for_queue().
 *
 * The queue runs one item at a time on a single thread at the lowest
 * application priority, so an item can wait for seconds behind another.
 * It is not for latency sensitive work. Never wait for an item of this
 * queue (k_work_cancel_*_sync(), k_work_flush()) from the system
 * workqueue or from this queue's own thread.
 */
extern struct k_work_q blocking_work_q;
int blocking_work_q_init(void);
#else
static inline int blocking_work_q_init(void) { return 0; }
#endif

#endif /* __WORKQ_H__ */
