/*
 * SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Included by TF-PSA-Crypto after Zephyr's configuration, through
 * CONFIG_TF_PSA_CRYPTO_USER_CONFIG_FILE.
 */

/* Compute X25519 shared secrets with the Project Everest code rather than
 * the generic ECP code. Key generation still uses the generic code.
 */
#define MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED
