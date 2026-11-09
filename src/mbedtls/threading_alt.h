/*
 * mbedTLS threading primitives, see crypto_threading.c
 *
 * SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __THREADING_ALT_H__
#define __THREADING_ALT_H__

#include <zephyr/kernel.h>

typedef struct k_mutex mbedtls_platform_mutex_t;
typedef struct k_condvar mbedtls_platform_condition_variable_t;

#endif
