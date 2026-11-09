/*
 * SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Included by TF-PSA-Crypto after Zephyr's configuration, through
 * CONFIG_TF_PSA_CRYPTO_USER_CONFIG_FILE.
 */

/*
 * mbedTLS is used from several threads (the HTTP server, websocket senders
 * and HTTPS certificate management), so it needs locking. The mutex and
 * condition variable implementation is in crypto_threading.c, and the
 * types are in threading_alt.h.
 */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT

#if defined(PSA_WANT_ECC_MONTGOMERY_255)
/* Compute X25519 shared secrets with the Project Everest code rather than
 * the generic ECP code. Key generation still uses the generic code.
 */
#define MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED
#endif
