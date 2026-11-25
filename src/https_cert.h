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
#include <zephyr/sys/util.h>

struct shell;

enum https_cert_source {
	HTTPS_CERT_SELF_SIGNED = 0,
	HTTPS_CERT_INSTALLED = 1,
};

/* Longest extra names list, comma separated */
#define HTTPS_CERT_EXTRA_NAMES_MAX	255

/* Most certificates in an installed chain, the BMC's certificate included */
#define HTTPS_CERT_CHAIN_COUNT_MAX	4

/*
 * Largest installed chain, as DER. The chain arrives as PEM in a request
 * that fits in a received TLS record, so it's at most 3/4 of one (and in
 * practice less, the request has more than the base64 in it). The BMC also
 * sends the chain in one TLS record, where each certificate has 5 bytes of
 * framing plus 8 bytes for the message (see https_cert_install()). With
 * the same size records for both, the received record is the limit. It
 * uses CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN and _OUT_CONTENT_LEN, so only
 * works with CONFIG_APP_HTTPS_PUBLIC_KEY.
 */
#define HTTPS_CERT_CHAIN_DER_MAX	MIN(CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN * 3 / 4,	\
					    CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN - 13)

/*
 * Largest certificate (the leaf, not the chain) that can be installed, as
 * DER: what fits in a TLS record, up to 2 KiB, which bounds the buffers the
 * certificate is shown through. A CA-issued ECDSA certificate is usually
 * under 1.5 KiB.
 */
#define HTTPS_CERT_LEAF_DER_MAX		MIN(2048, HTTPS_CERT_CHAIN_DER_MAX)

/* PEM of der_len bytes: base64, a newline per 48 bytes, BEGIN/END lines and NUL */
#define HTTPS_CERT_PEM_SIZE(der_len)	((der_len) * 4 / 3 + (der_len) / 48 + 64)

/*
 * The exact length of a certificate of der_len bytes as PEM, without the
 * NUL, and its number of lines: base64 in lines of 64 characters, from 48
 * bytes each, between "-----BEGIN CERTIFICATE-----" and
 * "-----END CERTIFICATE-----".
 */
#define HTTPS_CERT_PEM_LINES(der_len)	(DIV_ROUND_UP(der_len, 48) + 2)
#define HTTPS_CERT_PEM_LEN(der_len)	(27 + 4 * DIV_ROUND_UP(der_len, 3) + 25 +	\
					 HTTPS_CERT_PEM_LINES(der_len))

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

/*
 * Details of the current certificate, and the certificate as PEM, both of
 * the leaf if there's a chain.
 */
int https_cert_get(struct https_cert_details *details, char *pem, size_t size);

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
 * Checks a certificate before it's installed, given its details and the
 * length of its DER. Returns 0 if it can be installed, -E2BIG if its
 * details are too large to show, or another error.
 */
typedef int (*https_cert_check_t)(const struct https_cert_details *details, size_t der_len);

/*
 * Install a PEM certificate, optionally followed by its chain, issued for
 * the HTTPS key. pem is overwritten, the certificates are decoded over it.
 * check, if not NULL, is called for the leaf, with the lock held. It's
 * saved before it's used. On failure, including if it can't be saved,
 * the current certificate is unchanged and *err explains why.
 */
int https_cert_install(char *pem, https_cert_check_t check, const char **err);

/*
 * Go back to a self-signed certificate. If it can't be saved, the current
 * certificate is unchanged and an error returned.
 */
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
