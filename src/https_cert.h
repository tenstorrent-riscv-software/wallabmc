/*
 * SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __HTTPS_CERT_H__
#define __HTTPS_CERT_H__

#include <stddef.h>
#include <stdint.h>
#include <errno.h>

struct shell;

enum https_cert_source {
	HTTPS_CERT_SELF_SIGNED = 0,
	HTTPS_CERT_INSTALLED = 1,
};

#if defined(CONFIG_APP_HTTPS_PUBLIC_KEY)
/*
 * Load the HTTPS key and certificate from config, or generate a new key and
 * self-signed certificate if there are none.
 */
int https_cert_init(void);

/*
 * Get the certificate and private key (both DER) for the TLS credentials.
 * Returns -ENOENT if there are none. The buffers stay valid until the
 * certificate is replaced.
 */
int https_cert_get(const uint8_t **cert, size_t *cert_len,
		   const uint8_t **key, size_t *key_len);

int https_cert_cmd_show(const struct shell *sh, size_t argc, char **argv);
int https_cert_cmd_regenerate(const struct shell *sh, size_t argc, char **argv);
#else
static inline int https_cert_init(void)
{
	return 0;
}

static inline int https_cert_get(const uint8_t **cert, size_t *cert_len,
				 const uint8_t **key, size_t *key_len)
{
	return -ENOENT;
}
#endif

#endif
