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

/* Longest extra names list, comma separated */
#define HTTPS_CERT_EXTRA_NAMES_MAX	255

#if defined(CONFIG_APP_HTTPS_PUBLIC_KEY)
/*
 * Load the HTTPS key and certificate from config, or generate a new key and
 * self-signed certificate if there are none.
 */
int https_cert_init(void);

/*
 * Register the certificate and private key as TLS credentials under sec_tag.
 * They're kept up to date when the certificate is replaced. Returns -ENOENT
 * if there is no certificate.
 */
int https_cert_register(int sec_tag);

/*
 * The BMC's hostname or static IPv4 address changed. A self-signed
 * certificate is reissued for the new names with the same key.
 */
void https_cert_names_changed(void);

/*
 * Extra names (DNS names or IPv4 addresses, comma separated) to put in the
 * certificate besides the hostname and static IPv4 address, eg. the BMC's
 * fully qualified name. Returns -EINVAL if a name isn't valid.
 */
int https_cert_extra_names_set(const char *names);

int https_cert_cmd_show(const struct shell *sh, size_t argc, char **argv);
int https_cert_cmd_regenerate(const struct shell *sh, size_t argc, char **argv);
int https_cert_cmd_names(const struct shell *sh, size_t argc, char **argv);
#else
static inline int https_cert_init(void)
{
	return 0;
}

static inline int https_cert_register(int sec_tag)
{
	return -ENOENT;
}

static inline void https_cert_names_changed(void)
{
}
#endif

#endif
