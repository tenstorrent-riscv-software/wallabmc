/*
 * TF-PSA-Crypto options that Zephyr has no Kconfig for, included after
 * Zephyr's config-tf-psa-crypto.h.
 *
 * SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * mbedTLS is used from several threads (the HTTP server, websocket senders
 * and HTTPS certificate management), so it needs locking. The mutex and
 * condition variable implementation is in crypto_threading.c.
 */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT
