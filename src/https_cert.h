/*
 * SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __HTTPS_CERT_H__
#define __HTTPS_CERT_H__

#include <stdbool.h>
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

/*
 * Largest certificate (the leaf, not the chain) that can be installed, as
 * DER. A CA-issued ECDSA certificate is usually under 1.5 KiB. It bounds the
 * buffers the certificate is shown through.
 */
#define HTTPS_CERT_LEAF_DER_MAX		2048

/* PEM of der_len bytes: base64, a newline per 48 bytes, BEGIN/END lines and NUL */
#define HTTPS_CERT_PEM_SIZE(der_len)	((der_len) * 4 / 3 + (der_len) / 48 + 64)

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

/* Distinguished name attributes, empty if not present */
struct https_cert_dn {
	char cn[65];
	char o[65];
	char ou[65];
	char l[65];
	char st[65];
	char c[3];
	char email[65];
};

struct https_cert_details {
	struct https_cert_dn subject;
	struct https_cert_dn issuer;
	char not_before[21];		/* YYYY-MM-DDThh:mm:ssZ */
	char not_after[21];
	char fingerprint[96];		/* SHA-256, AA:BB:... */
	char serial[64];		/* AA:BB:... */
	bool digital_signature;
	bool key_agreement;
	bool server_auth;
	bool client_auth;
};

/* Details of the current certificate (the leaf, if there's a chain) */
int https_cert_details_get(struct https_cert_details *details);

/* The current certificate (the leaf, if there's a chain) as PEM */
int https_cert_pem_get(char *buf, size_t size);

struct https_cert_csr_params {
	const char *cn;			/* Required */
	const char *o;
	const char *ou;
	const char *l;
	const char *st;
	const char *c;
	const char *email;
	/* DNS names or IPv4 addresses, the current names if none */
	const char *const *alt_names;
	size_t alt_count;
};

/*
 * Make a PEM certificate signing request for the HTTPS key. On failure *err
 * explains why.
 */
int https_cert_csr_generate(const struct https_cert_csr_params *params,
			    char *pem, size_t size, const char **err);

/*
 * Install a PEM certificate, optionally followed by its chain, issued for
 * the HTTPS key. pem is modified. On failure *err explains why.
 */
int https_cert_install(char *pem, const char **err);

/* Go back to a self-signed certificate */
int https_cert_self_signed(void);

int https_cert_cmd_show(const struct shell *sh, size_t argc, char **argv);
int https_cert_cmd_self_signed(const struct shell *sh, size_t argc, char **argv);
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
