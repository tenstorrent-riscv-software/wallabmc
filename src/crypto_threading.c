/*
 * mbedTLS threading primitives on Zephyr mutexes and condition variables
 *
 * Zephyr builds mbedTLS without MBEDTLS_THREADING_C, so the PSA key store
 * and the mbedTLS heap have no locking. WallaBMC uses mbedTLS from several
 * threads, so enable it with MBEDTLS_THREADING_ALT (see
 * mbedtls/wallabmc_tf_psa_crypto_config.h) and provide the primitives here.
 *
 * SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <mbedtls/threading.h>

#if !defined(MBEDTLS_THREADING_C) || !defined(MBEDTLS_THREADING_ALT)
#error "MBEDTLS_THREADING_C and MBEDTLS_THREADING_ALT must be enabled"
#endif

static int mutex_init(mbedtls_platform_mutex_t *mutex)
{
	return k_mutex_init(mutex) ? MBEDTLS_ERR_THREADING_USAGE_ERROR : 0;
}

static void mutex_destroy(mbedtls_platform_mutex_t *mutex)
{
	ARG_UNUSED(mutex);
}

static int mutex_lock(mbedtls_platform_mutex_t *mutex)
{
	return k_mutex_lock(mutex, K_FOREVER) ? MBEDTLS_ERR_THREADING_USAGE_ERROR : 0;
}

static int mutex_unlock(mbedtls_platform_mutex_t *mutex)
{
	return k_mutex_unlock(mutex) ? MBEDTLS_ERR_THREADING_USAGE_ERROR : 0;
}

static int cond_init(mbedtls_platform_condition_variable_t *cond)
{
	return k_condvar_init(cond) ? MBEDTLS_ERR_THREADING_USAGE_ERROR : 0;
}

static void cond_destroy(mbedtls_platform_condition_variable_t *cond)
{
	ARG_UNUSED(cond);
}

static int cond_signal(mbedtls_platform_condition_variable_t *cond)
{
	return k_condvar_signal(cond) ? MBEDTLS_ERR_THREADING_USAGE_ERROR : 0;
}

static int cond_broadcast(mbedtls_platform_condition_variable_t *cond)
{
	k_condvar_broadcast(cond);
	return 0;
}

static int cond_wait(mbedtls_platform_condition_variable_t *cond,
		     mbedtls_platform_mutex_t *mutex)
{
	return k_condvar_wait(cond, mutex, K_FOREVER) ? MBEDTLS_ERR_THREADING_USAGE_ERROR : 0;
}

static int crypto_threading_init(void)
{
	mbedtls_threading_set_alt(mutex_init, mutex_destroy, mutex_lock, mutex_unlock,
				  cond_init, cond_destroy, cond_signal, cond_broadcast,
				  cond_wait);
	return 0;
}

/*
 * Must run before Zephyr's _mbedtls_init() (POST_KERNEL,
 * CONFIG_KERNEL_INIT_PRIORITY_DEFAULT), which initialises the heap and calls
 * psa_crypto_init(), both of which create mutexes.
 */
BUILD_ASSERT(CONFIG_KERNEL_INIT_PRIORITY_DEFAULT > 0);
SYS_INIT(crypto_threading_init, POST_KERNEL, 0);
