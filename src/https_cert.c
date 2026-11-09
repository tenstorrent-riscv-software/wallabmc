/*
 * HTTPS server key and certificate
 *
 * Each board generates its own P-256 key on first boot and keeps it in
 * config. The certificate is self-signed for the BMC's hostname, static
 * IPv4 address and any extra names, and is reissued with the same key when
 * they change.
 *
 * SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/base64.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

#include <psa/crypto.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <mbedtls/oid.h>
#include <mbedtls/asn1.h>
#include <mbedtls/platform_util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wallabmc_https_cert, LOG_LEVEL_INF);

#include "https_cert.h"
#include "config.h"
#include "main.h"
#include "net.h"

/* SEC1 ECPrivateKey DER for P-256 is 121 bytes */
#define KEY_DER_MAX		128
/* SubjectPublicKeyInfo DER for P-256 is 91 bytes */
#define SPKI_DER_MAX		96
/* Uncompressed P-256 point, the end of the SPKI */
#define EC_POINT_LEN		65
/*
 * The certificate credential is the leaf as DER, or the leaf and chain as
 * NUL terminated PEM. The whole chain has to fit in one TLS record (see
 * https_cert_install()), which is at most about 5.5 KiB as PEM.
 */
#define CERT_BUF_MAX		6144
/* Most certificates in an installed chain, leaf included */
#define CHAIN_MAX		4

#define SHA256_LEN		32
#define SHA1_LEN		20

#define CERT_NOT_BEFORE		"20000101000000"
#define CERT_NOT_AFTER		"99991231235959"
#define CERT_ORG		"WallaBMC"

/* Hostname, static IPv4 address and extra names */
#define EXTRA_NAMES_COUNT_MAX	8
#define SAN_MAX			(2 + EXTRA_NAMES_COUNT_MAX)

/* Recursive, and held for everything below */
static K_MUTEX_DEFINE(https_cert_lock);

/* Registered as the TLS private key credential */
static uint8_t key_der[KEY_DER_MAX];
static size_t key_len;

/*
 * The current certificate is cert_bufs[cert_cur] and is registered as the
 * TLS certificate credential. A new certificate is written to the other
 * buffer, so the current one stays valid until the credential is swapped.
 */
static uint8_t cert_bufs[2][CERT_BUF_MAX];
static int cert_cur;
static size_t cert_len;
static uint8_t cert_source;
#define cert_der	(cert_bufs[cert_cur])
#define cert_next	(cert_bufs[!cert_cur])

/* TLS credential tag, -1 until registered */
static int cred_tag = -1;

/* Kept static to keep it off the stack */
static mbedtls_x509_crt scratch_crt;

static char extra_names[HTTPS_CERT_EXTRA_NAMES_MAX + 1];

/* A name the certificate should be for */
struct san_entry {
	int type;		/* MBEDTLS_X509_SAN_DNS_NAME or _IP_ADDRESS */
	const uint8_t *p;	/* DNS name (not NUL terminated) or ip4 */
	size_t len;
	uint8_t ip4[4];
	bool hostname;		/* The BMC's hostname */
};

static struct san_entry sans[SAN_MAX];
static size_t san_count;

/* Cached for display, so showing the cert needs no crypto */
static struct {
	bool valid;
	char fingerprint[SHA256_LEN * 3];	/* AA:BB:..., NUL terminated */
	char pin[(SHA256_LEN + 2) / 3 * 4 + 1];
	char subject[96];
	char issuer[96];
	char names[320];
	char uncovered[160];			/* Names an installed cert lacks */
} info;

/*
 * Parse one name, a DNS name (wildcards allowed) or an IPv4 address, into
 * e. A DNS name isn't copied, e points into name. Returns -EINVAL if it's
 * neither.
 */
static int san_entry_parse(const char *name, size_t len, struct san_entry *e)
{
	struct in_addr addr;
	char tmp[INET_ADDRSTRLEN];

	memset(e, 0, sizeof(*e));

	if (len < sizeof(tmp)) {
		memcpy(tmp, name, len);
		tmp[len] = '\0';
		if (net_addr_pton(AF_INET, tmp, &addr) == 0) {
			e->type = MBEDTLS_X509_SAN_IP_ADDRESS;
			memcpy(e->ip4, &addr, sizeof(e->ip4));
			e->p = e->ip4;
			e->len = sizeof(e->ip4);
			return 0;
		}
	}

	if (!net_dns_name_valid(name, len, true))
		return -EINVAL;

	e->type = MBEDTLS_X509_SAN_DNS_NAME;
	e->p = (const uint8_t *)name;
	e->len = len;
	return 0;
}

/*
 * Parse a list of DNS names and IPv4 addresses separated by commas or
 * spaces into out (if not NULL). Returns the number of names, -EINVAL if
 * one isn't valid or -E2BIG if there are more than max.
 */
static int extra_names_parse(const char *names, struct san_entry *out, size_t max)
{
	struct san_entry tmp;
	const char *p = names;
	size_t n = 0;

	for (;;) {
		size_t len;

		p += strspn(p, ", ");
		if (!*p)
			break;
		len = strcspn(p, ", ");

		if (n == max)
			return -E2BIG;
		if (san_entry_parse(p, len, out ? &out[n] : &tmp))
			return -EINVAL;
		/* The ip4 pointer is to the entry, fix it for the copy */
		if (out && out[n].type == MBEDTLS_X509_SAN_IP_ADDRESS)
			out[n].p = out[n].ip4;
		n++;
		p += len;
	}

	return n;
}

static bool san_entry_eq(const struct san_entry *e, int type, const uint8_t *p, size_t len)
{
	if (e->type != type || e->len != len)
		return false;

	if (type == MBEDTLS_X509_SAN_DNS_NAME)
		return !strncasecmp((const char *)e->p, (const char *)p, len);

	return !memcmp(e->p, p, len);
}

/* Build sans[] from the hostname, static IPv4 address and extra names */
static void names_build(void)
{
	const char *hostname = config_bmc_hostname();
	uint32_t ip4 = config_bmc_default_ip4();
	struct san_entry *e = sans;
	int n;

	memset(sans, 0, sizeof(sans));

	e->type = MBEDTLS_X509_SAN_DNS_NAME;
	e->p = (const uint8_t *)hostname;
	e->len = strlen(hostname);
	e->hostname = true;
	e++;

	if (ip4) {
		e->type = MBEDTLS_X509_SAN_IP_ADDRESS;
		memcpy(e->ip4, &ip4, sizeof(e->ip4));
		e->p = e->ip4;
		e->len = sizeof(e->ip4);
		e++;
	}

	n = extra_names_parse(extra_names, e, ARRAY_SIZE(sans) - (e - sans));
	if (n < 0) {
		LOG_WRN("Ignoring invalid extra certificate names \"%s\"", extra_names);
		n = 0;
	}
	san_count = (e - sans) + n;

	/* Drop duplicates, eg. an extra name that's the same as the hostname */
	for (size_t i = 1; i < san_count; i++) {
		for (size_t j = 0; j < i; j++) {
			if (san_entry_eq(&sans[j], sans[i].type, sans[i].p, sans[i].len)) {
				memmove(&sans[i], &sans[i + 1], (san_count - i - 1) * sizeof(sans[0]));
				san_count--;
				/* The entries moved, so fix up the ip4 pointers */
				for (size_t k = i; k < san_count; k++) {
					if (sans[k].type == MBEDTLS_X509_SAN_IP_ADDRESS)
						sans[k].p = sans[k].ip4;
				}
				i--;
				break;
			}
		}
	}
}

/* Generate a new key into key_der, using cert_next as scratch */
static int key_generate(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	mbedtls_pk_context pk;
	psa_key_id_t id;
	psa_status_t status;
	int ret;

	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_ANY_HASH));
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);

	status = psa_generate_key(&attr, &id);
	if (status != PSA_SUCCESS) {
		LOG_ERR("Key generation failed (err=%d)", status);
		return -EIO;
	}

	/*
	 * psa_export_key() gives the bare private scalar, which
	 * mbedtls_pk_parse_key() (and so the TLS credential) can't use, so
	 * write SEC1 DER through pk instead.
	 */
	mbedtls_pk_init(&pk);
	ret = mbedtls_pk_wrap_psa(&pk, id);
	if (ret == 0)
		ret = mbedtls_pk_write_key_der(&pk, cert_next, CERT_BUF_MAX);
	mbedtls_pk_free(&pk);
	psa_destroy_key(id);

	if (ret < 0) {
		LOG_ERR("Key export failed (err=-0x%04x)", -ret);
		ret = -EIO;
	} else if (ret > sizeof(key_der)) {
		LOG_ERR("Key too large (%d bytes)", ret);
		ret = -ENOMEM;
	} else {
		/* The DER is written at the end of the buffer */
		memcpy(key_der, cert_next + CERT_BUF_MAX - ret, ret);
		key_len = ret;
		ret = 0;
	}

	mbedtls_platform_zeroize(cert_next, CERT_BUF_MAX);
	return ret;
}

static int key_load(mbedtls_pk_context *pk)
{
	int ret;

	mbedtls_pk_init(pk);
	ret = mbedtls_pk_parse_key(pk, key_der, key_len, NULL, 0);
	if (ret) {
		LOG_ERR("Key parse failed (err=-0x%04x)", -ret);
		mbedtls_pk_free(pk);
		return -EINVAL;
	}

	if (!mbedtls_pk_can_do_psa(pk, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
				   PSA_KEY_USAGE_SIGN_HASH)) {
		LOG_ERR("Key is not an ECDSA signing key");
		mbedtls_pk_free(pk);
		return -EINVAL;
	}

	return 0;
}

/* Get the SubjectPublicKeyInfo DER of the key */
static int key_spki(mbedtls_pk_context *pk, uint8_t *buf, size_t *len)
{
	uint8_t tmp[SPKI_DER_MAX + 32];
	int ret;

	ret = mbedtls_pk_write_pubkey_der(pk, tmp, sizeof(tmp));
	if (ret < 0) {
		LOG_ERR("Public key export failed (err=-0x%04x)", -ret);
		return -EIO;
	}
	if (ret > SPKI_DER_MAX || ret < EC_POINT_LEN)
		return -EINVAL;

	memcpy(buf, tmp + sizeof(tmp) - ret, ret);
	*len = ret;

	return 0;
}

static int sha_compute(psa_algorithm_t alg, const uint8_t *in, size_t in_len,
		       uint8_t *out, size_t out_size)
{
	size_t out_len;
	psa_status_t status;

	status = psa_hash_compute(alg, in, in_len, out, out_size, &out_len);
	if (status != PSA_SUCCESS || out_len != out_size)
		return -EIO;

	return 0;
}

/* Issue a self-signed certificate for sans[] into cert_next */
static int cert_issue(mbedtls_pk_context *pk, const uint8_t *spki, size_t spki_len,
		      size_t *len)
{
	static mbedtls_x509_san_list san_list[SAN_MAX];
	mbedtls_x509write_cert crt;
	mbedtls_asn1_sequence eku;
	uint8_t serial[16];
	uint8_t ski[2 + SHA1_LEN];
	char subject[sizeof("CN=,O=" CERT_ORG) + CONFIG_NET_HOSTNAME_MAX_LEN];
	int ret;

	mbedtls_x509write_crt_init(&crt);
	mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
	mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
	mbedtls_x509write_crt_set_subject_key(&crt, pk);
	mbedtls_x509write_crt_set_issuer_key(&crt, pk);

	/*
	 * Random so browsers don't confuse a reissued cert with a cached one.
	 * The serial is written as is, so it must be positive and have no
	 * leading zero byte to be valid DER.
	 */
	if (psa_generate_random(serial, sizeof(serial)) != PSA_SUCCESS) {
		ret = -EIO;
		goto out;
	}
	serial[0] &= 0x7f;
	if (serial[0] == 0)
		serial[0] = 1;
	ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
	if (ret)
		goto out;

	/* sans[0] is the hostname */
	ret = snprintf(subject, sizeof(subject), "CN=%.*s,O=" CERT_ORG,
		       (int)sans[0].len, sans[0].p);
	if (ret < 0 || ret >= sizeof(subject)) {
		ret = -ENAMETOOLONG;
		goto out;
	}
	ret = mbedtls_x509write_crt_set_subject_name(&crt, subject);
	if (ret)
		goto out;
	ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject);
	if (ret)
		goto out;

	/* No trusted clock at first boot, so no expiry (RFC 5280 4.1.2.5) */
	ret = mbedtls_x509write_crt_set_validity(&crt, CERT_NOT_BEFORE, CERT_NOT_AFTER);
	if (ret)
		goto out;

	/*
	 * mbedtls_x509write_crt_set_basic_constraints() only marks the
	 * extension critical for a CA, write a critical CA:FALSE directly.
	 */
	ret = mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_BASIC_CONSTRAINTS,
						  MBEDTLS_OID_SIZE(MBEDTLS_OID_BASIC_CONSTRAINTS),
						  1, (const uint8_t *)"\x30\x00", 2);
	if (ret)
		goto out;

	ret = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
	if (ret)
		goto out;

	memset(&eku, 0, sizeof(eku));
	eku.buf.tag = MBEDTLS_ASN1_OID;
	eku.buf.p = (unsigned char *)MBEDTLS_OID_SERVER_AUTH;
	eku.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
	ret = mbedtls_x509write_crt_set_ext_key_usage(&crt, &eku);
	if (ret)
		goto out;

	/*
	 * SHA-1 of the public key BIT STRING, RFC 5280 4.2.1.2 (1).
	 * mbedtls_x509write_crt_set_subject_key_identifier() does this too,
	 * but with a 2 KiB buffer on the stack.
	 */
	ski[0] = MBEDTLS_ASN1_OCTET_STRING;
	ski[1] = SHA1_LEN;
	ret = sha_compute(PSA_ALG_SHA_1, spki + spki_len - EC_POINT_LEN, EC_POINT_LEN,
			  ski + 2, SHA1_LEN);
	if (ret)
		goto out;
	ret = mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_SUBJECT_KEY_IDENTIFIER,
						  MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_KEY_IDENTIFIER),
						  0, ski, sizeof(ski));
	if (ret)
		goto out;

	/*
	 * The names are written in the reverse order of the list, so link it
	 * backwards to keep the hostname first.
	 */
	memset(san_list, 0, sizeof(san_list));
	for (size_t i = 0; i < san_count; i++) {
		san_list[i].node.type = sans[i].type;
		san_list[i].node.san.unstructured_name.p = (unsigned char *)sans[i].p;
		san_list[i].node.san.unstructured_name.len = sans[i].len;
		san_list[i].next = i ? &san_list[i - 1] : NULL;
	}
	ret = mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san_list[san_count - 1]);
	if (ret)
		goto out;

	ret = mbedtls_x509write_crt_der(&crt, cert_next, CERT_BUF_MAX);
	if (ret < 0)
		goto out;

	/* The DER is written at the end of the buffer */
	memmove(cert_next, cert_next + CERT_BUF_MAX - ret, ret);
	*len = ret;
	ret = 0;

out:
	mbedtls_x509write_crt_free(&crt);
	if (ret)
		LOG_ERR("Certificate creation failed (err=%s%x)",
			ret < 0 ? "-0x" : "0x", ret < 0 ? -ret : ret);
	return ret ? -EIO : 0;
}

static bool san_is(const mbedtls_x509_buf *san, int type)
{
	return san->tag == (MBEDTLS_ASN1_CONTEXT_SPECIFIC | type);
}

static int san_type(const mbedtls_x509_buf *san)
{
	return san->tag & ~MBEDTLS_ASN1_CONTEXT_SPECIFIC;
}

/* True if the certificate's SANs are exactly sans[] */
static bool cert_names_equal(const mbedtls_x509_crt *crt)
{
	const mbedtls_x509_sequence *cur;
	bool matched[SAN_MAX] = { false };
	size_t count = 0;

	for (cur = &crt->subject_alt_names; cur && cur->buf.p; cur = cur->next) {
		size_t i;

		for (i = 0; i < san_count; i++) {
			if (!matched[i] &&
			    san_entry_eq(&sans[i], san_type(&cur->buf), cur->buf.p, cur->buf.len))
				break;
		}
		if (i == san_count)
			return false;
		matched[i] = true;
		count++;
	}

	return count == san_count;
}

/* True if the DNS SAN san covers the name, which may be a wildcard SAN */
static bool san_dns_covers(const mbedtls_x509_buf *san, const struct san_entry *e)
{
	const char *name = (const char *)e->p;
	const char *dot;

	if (san->len == e->len && !strncasecmp((const char *)san->p, name, e->len))
		return true;

	/* "*.example.com" covers one label in front of "example.com" */
	dot = memchr(name, '.', e->len);
	if (dot && dot != name && san->len > 2 && san->p[0] == '*' && san->p[1] == '.' &&
	    san->len - 1 == e->len - (dot - name) &&
	    !strncasecmp((const char *)san->p + 1, dot, san->len - 1))
		return true;

	/* The hostname is covered by a fully qualified name starting with it */
	if (e->hostname && san->len > e->len && san->p[e->len] == '.' &&
	    !strncasecmp((const char *)san->p, name, e->len))
		return true;

	return false;
}

/* List the names in sans[] that the certificate doesn't cover in info */
static void cert_names_check(const mbedtls_x509_crt *crt)
{
	char *buf = info.uncovered;
	size_t size = sizeof(info.uncovered);

	buf[0] = '\0';

	for (size_t i = 0; i < san_count; i++) {
		const struct san_entry *e = &sans[i];
		const mbedtls_x509_sequence *cur;
		size_t len = strlen(buf);
		bool covered = false;

		for (cur = &crt->subject_alt_names; cur && cur->buf.p && !covered; cur = cur->next) {
			if (e->type == MBEDTLS_X509_SAN_DNS_NAME &&
			    san_is(&cur->buf, MBEDTLS_X509_SAN_DNS_NAME))
				covered = san_dns_covers(&cur->buf, e);
			else
				covered = san_entry_eq(e, san_type(&cur->buf),
						       cur->buf.p, cur->buf.len);
		}
		if (covered)
			continue;

		if (e->type == MBEDTLS_X509_SAN_DNS_NAME)
			snprintf(buf + len, size - len, "%s%.*s", len ? ", " : "",
				 (int)e->len, e->p);
		else
			snprintf(buf + len, size - len, "%s%u.%u.%u.%u", len ? ", " : "",
				 e->p[0], e->p[1], e->p[2], e->p[3]);
	}
}

static void names_append(char *buf, size_t size, const mbedtls_x509_buf *san)
{
	size_t len = strlen(buf);
	const char *sep = len ? ", " : "";

	if (san_is(san, MBEDTLS_X509_SAN_DNS_NAME)) {
		snprintf(buf + len, size - len, "%sDNS:%.*s", sep, (int)san->len, san->p);
	} else if (san_is(san, MBEDTLS_X509_SAN_IP_ADDRESS) && san->len == 4) {
		snprintf(buf + len, size - len, "%sIP:%u.%u.%u.%u", sep,
			 san->p[0], san->p[1], san->p[2], san->p[3]);
	} else {
		snprintf(buf + len, size - len, "%s(type %d)", sep, san_type(san));
	}
}

/* Parse a certificate, DER or a NUL terminated PEM chain */
static int cert_parse(mbedtls_x509_crt *crt, const uint8_t *buf, size_t len)
{
	int ret;

	mbedtls_x509_crt_init(crt);
	ret = mbedtls_x509_crt_parse(crt, buf, len);
	if (ret) {
		LOG_ERR("Certificate parse failed (err=-0x%04x)", -ret);
		mbedtls_x509_crt_free(crt);
		return -EINVAL;
	}

	return 0;
}

static void info_update(const uint8_t *spki, size_t spki_len)
{
	mbedtls_x509_crt *crt = &scratch_crt;
	const mbedtls_x509_sequence *cur;
	uint8_t hash[SHA256_LEN];
	size_t olen;
	int ret;

	memset(&info, 0, sizeof(info));

	if (cert_parse(crt, cert_der, cert_len))
		return;

	/* Of the leaf, the first in a chain */
	if (sha_compute(PSA_ALG_SHA_256, crt->raw.p, crt->raw.len, hash, sizeof(hash)))
		goto out;
	for (int i = 0; i < sizeof(hash); i++)
		sprintf(info.fingerprint + i * 3 - (i ? 1 : 0), i ? ":%02X" : "%02X", hash[i]);

	/* Public key pin, as used by curl --pinnedpubkey sha256//<pin> */
	if (sha_compute(PSA_ALG_SHA_256, spki, spki_len, hash, sizeof(hash)))
		goto out;
	ret = base64_encode((uint8_t *)info.pin, sizeof(info.pin), &olen, hash, sizeof(hash));
	if (ret)
		goto out;

	ret = mbedtls_x509_dn_gets(info.subject, sizeof(info.subject), &crt->subject);
	if (ret < 0)
		strcpy(info.subject, "?");
	ret = mbedtls_x509_dn_gets(info.issuer, sizeof(info.issuer), &crt->issuer);
	if (ret < 0)
		strcpy(info.issuer, "?");

	for (cur = &crt->subject_alt_names; cur && cur->buf.p; cur = cur->next)
		names_append(info.names, sizeof(info.names), &cur->buf);

	if (cert_source == HTTPS_CERT_INSTALLED)
		cert_names_check(crt);

	info.valid = true;
out:
	mbedtls_x509_crt_free(crt);
}

static void info_log(void)
{
	if (!info.valid)
		return;

	LOG_INF("HTTPS certificate (%s): %s",
		cert_source == HTTPS_CERT_INSTALLED ? "installed" : "self-signed",
		info.subject);
	if (cert_source == HTTPS_CERT_INSTALLED)
		LOG_INF("  Issuer: %s", info.issuer);
	LOG_INF("  Names: %s", info.names);
	LOG_INF("  SHA-256 fingerprint: %s", info.fingerprint);
	LOG_INF("  Public key pin: sha256//%s", info.pin);
	if (info.uncovered[0])
		LOG_WRN("HTTPS certificate is not valid for: %s", info.uncovered);
}

/*
 * Check a stored certificate can be used with the key. A self-signed
 * certificate must also be for the current names, otherwise it's reissued.
 */
static bool cert_usable(const uint8_t *spki, size_t spki_len)
{
	mbedtls_x509_crt *crt = &scratch_crt;
	bool ok = true;

	if (cert_parse(crt, cert_der, cert_len))
		return false;

	if (crt->pk_raw.len != spki_len || memcmp(crt->pk_raw.p, spki, spki_len)) {
		LOG_WRN("Stored certificate is not for the stored key");
		ok = false;
	} else if (cert_source == HTTPS_CERT_SELF_SIGNED && !cert_names_equal(crt)) {
		LOG_INF("Hostname or addresses changed, reissuing certificate");
		ok = false;
	}

	mbedtls_x509_crt_free(crt);
	return ok;
}

static void save(enum config_blob blob, const void *buf, size_t len)
{
	if (config_blob_write(blob, buf, len))
		LOG_ERR("Could not save HTTPS %s",
			blob == CONFIG_BLOB_HTTPS_KEY ? "key" : "certificate");
}

/*
 * Make cert_next, of length len, the current certificate, and swap the TLS
 * credential to it if it's registered.
 */
static int cert_switch(size_t len, enum https_cert_source source)
{
	int ret;

	if (cred_tag >= 0) {
		/*
		 * There's no replace, the old one has to be deleted first. A
		 * connection accepted in between fails its handshake. Once
		 * delete returns, no connection is still reading the old
		 * buffer, they make their own copy.
		 */
		ret = tls_credential_delete(cred_tag, TLS_CREDENTIAL_PUBLIC_CERTIFICATE);
		if (ret && ret != -ENOENT)
			LOG_WRN("Could not delete certificate credential (err=%d)", ret);
		ret = tls_credential_add(cred_tag, TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
					 cert_next, len);
		if (ret) {
			LOG_ERR("Could not register new certificate (err=%d)", ret);
			/* Put the old one back */
			tls_credential_add(cred_tag, TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
					   cert_der, cert_len);
			return ret;
		}
	}

	cert_cur = !cert_cur;
	cert_len = len;
	cert_source = source;

	return 0;
}

/* Issue a self-signed certificate for sans[] and make it current */
static int cert_reissue(void)
{
	mbedtls_pk_context pk;
	uint8_t spki[SPKI_DER_MAX];
	size_t spki_len, len;
	uint32_t t;
	int ret;

	ret = key_load(&pk);
	if (ret)
		return ret;

	ret = key_spki(&pk, spki, &spki_len);
	if (ret)
		goto out;

	t = k_uptime_get_32();
	ret = cert_issue(&pk, spki, spki_len, &len);
	if (ret)
		goto out;

	ret = cert_switch(len, HTTPS_CERT_SELF_SIGNED);
	if (ret)
		goto out;

	LOG_INF("Issued self-signed HTTPS certificate in %u ms", k_uptime_get_32() - t);
	save(CONFIG_BLOB_HTTPS_CERT, cert_der, cert_len);
	save(CONFIG_BLOB_HTTPS_CERT_SOURCE, &cert_source, sizeof(cert_source));

	info_update(spki, spki_len);
	info_log();

out:
	mbedtls_pk_free(&pk);
	return ret;
}

static void extra_names_load(void)
{
	ssize_t rc;

	rc = config_blob_read(CONFIG_BLOB_HTTPS_EXTRA_NAMES, extra_names,
			      sizeof(extra_names) - 1);
	extra_names[rc > 0 ? rc : 0] = '\0';

	/*
	 * https_cert_extra_names_set() only saves valid names, so these are
	 * corrupt or left over from something else. Drop them, rather than
	 * warning every time the certificate is issued.
	 */
	if (extra_names_parse(extra_names, NULL, EXTRA_NAMES_COUNT_MAX) < 0) {
		LOG_WRN("Deleting invalid extra certificate names from config");
		extra_names[0] = '\0';
		config_blob_delete(CONFIG_BLOB_HTTPS_EXTRA_NAMES);
	}
}

static int https_cert_load(void)
{
	mbedtls_pk_context pk;
	uint8_t spki[SPKI_DER_MAX];
	size_t spki_len;
	uint32_t t;
	ssize_t rc;
	int ret;

	if (!config_persistent())
		LOG_WRN("No persistent storage, the HTTPS key will change on every boot");

	extra_names_load();
	names_build();

	rc = config_blob_read(CONFIG_BLOB_HTTPS_KEY, key_der, sizeof(key_der));
	key_len = rc > 0 ? rc : 0;
	if (key_len) {
		ret = key_load(&pk);
		if (ret) {
			LOG_ERR("Stored HTTPS key is invalid, generating a new one");
			key_len = 0;
		} else {
			mbedtls_pk_free(&pk);
		}
	} else if (rc != -ENOENT && rc != -ENODEV) {
		LOG_ERR("Could not read HTTPS key (err=%zd), generating a new one", rc);
	}

	cert_len = 0;
	if (!key_len) {
		LOG_INF("Generating HTTPS key");
		t = k_uptime_get_32();
		ret = key_generate();
		if (ret)
			return ret;
		LOG_INF("Generated HTTPS key in %u ms", k_uptime_get_32() - t);
		save(CONFIG_BLOB_HTTPS_KEY, key_der, key_len);
	} else {
		rc = config_blob_read(CONFIG_BLOB_HTTPS_CERT, cert_der, CERT_BUF_MAX);
		if (rc > 0) {
			cert_len = rc;
			if (config_blob_read(CONFIG_BLOB_HTTPS_CERT_SOURCE, &cert_source,
					     sizeof(cert_source)) != sizeof(cert_source))
				cert_source = HTTPS_CERT_SELF_SIGNED;
		}
	}

	ret = key_load(&pk);
	if (ret)
		return ret;
	ret = key_spki(&pk, spki, &spki_len);
	mbedtls_pk_free(&pk);
	if (ret)
		return ret;

	if (cert_len && cert_usable(spki, spki_len)) {
		info_update(spki, spki_len);
		info_log();
		return 0;
	}

	cert_len = 0;
	return cert_reissue();
}

int https_cert_init(void)
{
	int ret;

	k_mutex_lock(&https_cert_lock, K_FOREVER);
	ret = https_cert_load();
	if (ret)
		cert_len = 0;
	k_mutex_unlock(&https_cert_lock);

	if (ret)
		LOG_ERR("No HTTPS certificate (err=%d), HTTPS is not available", ret);

#if defined(CONFIG_THREAD_STACK_INFO) && defined(CONFIG_INIT_STACKS)
	size_t unused;

	if (!k_thread_stack_space_get(k_current_get(), &unused))
		LOG_DBG("Stack unused after HTTPS cert init: %zu bytes", unused);
#endif

	return ret;
}

int https_cert_register(int sec_tag)
{
	int ret;

	k_mutex_lock(&https_cert_lock, K_FOREVER);

	if (!cert_len || !key_len) {
		ret = -ENOENT;
		goto out;
	}

	ret = tls_credential_add(sec_tag, TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
				 cert_der, cert_len);
	if (ret) {
		LOG_ERR("Failed to register certificate (err=%d)", ret);
		goto out;
	}

	ret = tls_credential_add(sec_tag, TLS_CREDENTIAL_PRIVATE_KEY, key_der, key_len);
	if (ret) {
		LOG_ERR("Failed to register private key (err=%d)", ret);
		tls_credential_delete(sec_tag, TLS_CREDENTIAL_PUBLIC_CERTIFICATE);
		goto out;
	}

	cred_tag = sec_tag;
out:
	k_mutex_unlock(&https_cert_lock);
	return ret;
}

/* Called with https_cert_lock held */
static void names_changed(void)
{
	mbedtls_x509_crt *crt = &scratch_crt;
	bool equal;

	/* Not initialised, or it failed */
	if (!key_len || !cert_len)
		return;

	names_build();

	if (cert_source == HTTPS_CERT_INSTALLED) {
		if (cert_parse(crt, cert_der, cert_len))
			return;
		cert_names_check(crt);
		mbedtls_x509_crt_free(crt);
		if (info.uncovered[0])
			LOG_WRN("HTTPS certificate is not valid for: %s", info.uncovered);
		return;
	}

	if (cert_parse(crt, cert_der, cert_len))
		return;
	equal = cert_names_equal(crt);
	mbedtls_x509_crt_free(crt);

	if (!equal)
		cert_reissue();
}

void https_cert_names_changed(void)
{
	k_mutex_lock(&https_cert_lock, K_FOREVER);
	names_changed();
	k_mutex_unlock(&https_cert_lock);

#if defined(CONFIG_THREAD_STACK_INFO) && defined(CONFIG_INIT_STACKS)
	size_t unused;

	if (!k_thread_stack_space_get(k_current_get(), &unused))
		LOG_DBG("Stack unused after HTTPS cert reissue: %zu bytes", unused);
#endif
}

int https_cert_extra_names_set(const char *names)
{
	char normalised[HTTPS_CERT_EXTRA_NAMES_MAX + 1];
	const char *p = names;
	size_t len = 0;
	int ret;

	ret = extra_names_parse(names, NULL, EXTRA_NAMES_COUNT_MAX);
	if (ret < 0)
		return ret;

	/* Store them comma separated */
	for (;;) {
		size_t n;

		p += strspn(p, ", ");
		if (!*p)
			break;
		n = strcspn(p, ", ");
		if (len + !!len + n >= sizeof(normalised))
			return -E2BIG;
		if (len)
			normalised[len++] = ',';
		memcpy(normalised + len, p, n);
		len += n;
		p += n;
	}
	normalised[len] = '\0';

	k_mutex_lock(&https_cert_lock, K_FOREVER);
	ret = len ? config_blob_write(CONFIG_BLOB_HTTPS_EXTRA_NAMES, normalised, len) :
		    config_blob_delete(CONFIG_BLOB_HTTPS_EXTRA_NAMES);
	if (!ret) {
		strcpy(extra_names, normalised);
		names_changed();
	}
	k_mutex_unlock(&https_cert_lock);

	return ret;
}

int https_cert_cmd_show(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_mutex_lock(&https_cert_lock, K_FOREVER);
	if (!cert_len || !info.valid) {
		k_mutex_unlock(&https_cert_lock);
		shell_print(sh, "No HTTPS certificate");
		return -ENOENT;
	}

	shell_print(sh, "Source:              %s",
		    cert_source == HTTPS_CERT_INSTALLED ? "installed" : "self-signed");
	shell_print(sh, "Subject:             %s", info.subject);
	if (cert_source == HTTPS_CERT_INSTALLED)
		shell_print(sh, "Issuer:              %s", info.issuer);
	shell_print(sh, "Names:               %s", info.names);
	shell_print(sh, "SHA-256 fingerprint: %s", info.fingerprint);
	shell_print(sh, "Public key pin:      sha256//%s", info.pin);
	shell_print(sh, "Extra names:         %s", extra_names[0] ? extra_names : "(none)");
	if (info.uncovered[0])
		shell_warn(sh, "Certificate is not valid for: %s", info.uncovered);
	k_mutex_unlock(&https_cert_lock);

	return 0;
}

int https_cert_cmd_regenerate(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!is_boot_finished()) {
		shell_error(sh, "must wait for boot to finish");
		return -EAGAIN;
	}

	if (!config_persistent()) {
		shell_error(sh, "No persistent storage, a new key is generated on every boot");
		return -ENODEV;
	}

	/*
	 * The key is also registered as a TLS credential, and connections
	 * could be using it. Delete it so a new one is generated on the next
	 * boot instead.
	 */
	rc = config_blob_delete(CONFIG_BLOB_HTTPS_KEY);
	if (!rc)
		rc = config_blob_delete(CONFIG_BLOB_HTTPS_CERT);
	if (!rc)
		rc = config_blob_delete(CONFIG_BLOB_HTTPS_CERT_SOURCE);
	if (rc) {
		shell_error(sh, "Could not delete the HTTPS key (err=%d)", rc);
		return rc;
	}

	shell_print(sh, "HTTPS key deleted, rebooting to generate a new one");
	bmc_reboot();

	return 0;
}

int https_cert_cmd_names(const struct shell *sh, size_t argc, char **argv)
{
	char names[HTTPS_CERT_EXTRA_NAMES_MAX + 1];
	size_t len = 0;
	int rc;

	if (argc < 2) {
		shell_print(sh, "Extra names: %s", extra_names[0] ? extra_names : "(none)");
		return 0;
	}

	if (!is_boot_finished()) {
		shell_error(sh, "must wait for boot to finish");
		return -EAGAIN;
	}

	/* "bmc cert names clear" removes them */
	names[0] = '\0';
	if (!(argc == 2 && !strcmp(argv[1], "clear"))) {
		for (size_t i = 1; i < argc; i++) {
			rc = snprintf(names + len, sizeof(names) - len, "%s%s",
				      len ? "," : "", argv[i]);
			if (rc < 0 || rc >= sizeof(names) - len) {
				shell_error(sh, "Too many names");
				return -E2BIG;
			}
			len += rc;
		}
	}

	rc = https_cert_extra_names_set(names);
	if (rc == -EINVAL) {
		shell_error(sh, "Invalid name, use DNS names or IPv4 addresses");
		return rc;
	} else if (rc == -E2BIG) {
		shell_error(sh, "Too many names, at most %d", EXTRA_NAMES_COUNT_MAX);
		return rc;
	} else if (rc) {
		shell_error(sh, "Could not set names (err=%d)", rc);
		return rc;
	}

	shell_print(sh, "Extra names: %s", extra_names[0] ? extra_names : "(none)");
	return 0;
}

/*
 * Append der as a PEM block with the given label to buf at *pos, keeping
 * buf NUL terminated.
 */
static int pem_append(char *buf, size_t size, size_t *pos, const char *label,
		      const uint8_t *der, size_t der_len)
{
	size_t olen;
	int ret;

	ret = snprintf(buf + *pos, size - *pos, "-----BEGIN %s-----\n", label);
	if (ret < 0 || ret >= size - *pos)
		return -ENOMEM;
	*pos += ret;

	/* 48 bytes is 64 characters, the PEM line length */
	for (size_t off = 0; off < der_len; off += 48) {
		if (base64_encode((uint8_t *)buf + *pos, size - *pos, &olen,
				  der + off, MIN(48, der_len - off)))
			return -ENOMEM;
		*pos += olen;
		if (*pos + 2 > size)
			return -ENOMEM;
		buf[(*pos)++] = '\n';
		buf[*pos] = '\0';
	}

	ret = snprintf(buf + *pos, size - *pos, "-----END %s-----\n", label);
	if (ret < 0 || ret >= size - *pos)
		return -ENOMEM;
	*pos += ret;

	return 0;
}

#define PEM_BEGIN_CRT	"-----BEGIN CERTIFICATE-----"
#define PEM_END_CRT	"-----END CERTIFICATE-----"

/*
 * Decode the next PEM certificate after *pp into der. The base64 is
 * compacted in place to drop line breaks. Returns -ENOENT if there are no
 * more.
 */
static int pem_next_cert(char **pp, uint8_t *der, size_t size, size_t *len)
{
	char *begin, *end, *w;

	begin = strstr(*pp, PEM_BEGIN_CRT);
	if (!begin)
		return -ENOENT;
	begin += strlen(PEM_BEGIN_CRT);

	end = strstr(begin, PEM_END_CRT);
	if (!end)
		return -EINVAL;
	*pp = end + strlen(PEM_END_CRT);

	w = begin;
	for (const char *r = begin; r < end; r++) {
		if (!isspace((unsigned char)*r))
			*w++ = *r;
	}

	if (base64_decode(der, size, len, (const uint8_t *)begin, w - begin))
		return w - begin > size * 4 / 3 ? -ENOMEM : -EINVAL;

	return 0;
}

static void dn_get(const mbedtls_x509_name *name, struct https_cert_dn *dn)
{
	static const struct {
		const char *oid;
		size_t oid_len;
		size_t offset;
		size_t size;
	} attrs[] = {
#define ATTR(oid, field) { oid, MBEDTLS_OID_SIZE(oid), offsetof(struct https_cert_dn, field), \
			   sizeof(((struct https_cert_dn *)0)->field) }
		ATTR(MBEDTLS_OID_AT_CN, cn),
		ATTR(MBEDTLS_OID_AT_ORGANIZATION, o),
		ATTR(MBEDTLS_OID_AT_ORG_UNIT, ou),
		ATTR(MBEDTLS_OID_AT_LOCALITY, l),
		ATTR(MBEDTLS_OID_AT_STATE, st),
		ATTR(MBEDTLS_OID_AT_COUNTRY, c),
		ATTR(MBEDTLS_OID_PKCS9_EMAIL, email),
#undef ATTR
	};

	memset(dn, 0, sizeof(*dn));

	for (; name; name = name->next) {
		for (size_t i = 0; i < ARRAY_SIZE(attrs); i++) {
			char *field = (char *)dn + attrs[i].offset;

			if (name->oid.len != attrs[i].oid_len ||
			    memcmp(name->oid.p, attrs[i].oid, attrs[i].oid_len) || field[0])
				continue;
			snprintf(field, attrs[i].size, "%.*s", (int)name->val.len, name->val.p);
		}
	}
}

static void time_get(const mbedtls_x509_time *t, char *buf, size_t size)
{
	snprintf(buf, size, "%04d-%02d-%02dT%02d:%02d:%02dZ",
		 t->year, t->mon, t->day, t->hour, t->min, t->sec);
}

int https_cert_details_get(struct https_cert_details *d)
{
	mbedtls_x509_crt *crt = &scratch_crt;
	size_t len = 0;
	int ret;

	memset(d, 0, sizeof(*d));

	k_mutex_lock(&https_cert_lock, K_FOREVER);

	if (!cert_len || !info.valid) {
		ret = -ENOENT;
		goto out;
	}

	ret = cert_parse(crt, cert_der, cert_len);
	if (ret)
		goto out;

	dn_get(&crt->subject, &d->subject);
	dn_get(&crt->issuer, &d->issuer);
	time_get(&crt->valid_from, d->not_before, sizeof(d->not_before));
	time_get(&crt->valid_to, d->not_after, sizeof(d->not_after));
	strcpy(d->fingerprint, info.fingerprint);

	for (size_t i = 0; i < crt->serial.len && len + 3 < sizeof(d->serial); i++)
		len += sprintf(d->serial + len, i ? ":%02X" : "%02X", crt->serial.p[i]);

	d->digital_signature = !mbedtls_x509_crt_check_key_usage(crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
	d->key_agreement = !mbedtls_x509_crt_check_key_usage(crt, MBEDTLS_X509_KU_KEY_AGREEMENT);
	d->server_auth = !mbedtls_x509_crt_check_extended_key_usage(crt, MBEDTLS_OID_SERVER_AUTH,
					MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH));
	d->client_auth = !mbedtls_x509_crt_check_extended_key_usage(crt, MBEDTLS_OID_CLIENT_AUTH,
					MBEDTLS_OID_SIZE(MBEDTLS_OID_CLIENT_AUTH));

	mbedtls_x509_crt_free(crt);
out:
	k_mutex_unlock(&https_cert_lock);
	return ret;
}

int https_cert_pem_get(char *buf, size_t size)
{
	mbedtls_x509_crt *crt = &scratch_crt;
	size_t pos = 0;
	int ret;

	k_mutex_lock(&https_cert_lock, K_FOREVER);

	if (!cert_len) {
		ret = -ENOENT;
		goto out;
	}

	ret = cert_parse(crt, cert_der, cert_len);
	if (ret)
		goto out;

	/* Just the leaf */
	buf[0] = '\0';
	ret = pem_append(buf, size, &pos, "CERTIFICATE", crt->raw.p, crt->raw.len);
	mbedtls_x509_crt_free(crt);
out:
	k_mutex_unlock(&https_cert_lock);
	return ret;
}

/* Explains the last failure of https_cert_csr_generate() or https_cert_install() */
static char err_msg[192];

#define fail(ret_, ...)						\
	({							\
		snprintf(err_msg, sizeof(err_msg), __VA_ARGS__);\
		LOG_WRN("%s", err_msg);				\
		ret_;						\
	})

/* Append "key=value," to a subject name, escaping the value */
static int rdn_append(char *buf, size_t size, size_t *pos, const char *key, const char *val)
{
	int ret;

	if (!val || !val[0])
		return 0;

	ret = snprintf(buf + *pos, size - *pos, "%s%s=", *pos ? "," : "", key);
	if (ret < 0 || ret >= size - *pos)
		return -ENAMETOOLONG;
	*pos += ret;

	for (; *val; val++) {
		if (*pos + 3 > size)
			return -ENAMETOOLONG;
		if (strchr(",=+<>#;\"\\", *val))
			buf[(*pos)++] = '\\';
		buf[(*pos)++] = *val;
	}
	buf[*pos] = '\0';

	return 0;
}

int https_cert_csr_generate(const struct https_cert_csr_params *params,
			    char *pem, size_t size, const char **err)
{
	static struct san_entry csr_sans[SAN_MAX + 1];
	static mbedtls_x509_san_list san_list[SAN_MAX + 1];
	static char subject[512];
	mbedtls_x509write_csr csr;
	mbedtls_pk_context pk;
	size_t pos = 0, count = 0;
	struct san_entry cn_entry;
	bool have_cn = false;
	int ret;

	*err = err_msg;

	if (!params->cn || !params->cn[0])
		return fail(-EINVAL, "CommonName is required");
	if (params->c && params->c[0] &&
	    (strlen(params->c) != 2 || !isalpha((unsigned char)params->c[0]) ||
	     !isalpha((unsigned char)params->c[1])))
		return fail(-EINVAL, "Country must be a two letter code");

	k_mutex_lock(&https_cert_lock, K_FOREVER);

	if (!key_len) {
		ret = fail(-ENOENT, "No HTTPS key");
		goto out_unlock;
	}

	/* Most significant first, as they're usually shown */
	subject[0] = '\0';
	ret = rdn_append(subject, sizeof(subject), &pos, "C", params->c);
	if (!ret)
		ret = rdn_append(subject, sizeof(subject), &pos, "ST", params->st);
	if (!ret)
		ret = rdn_append(subject, sizeof(subject), &pos, "L", params->l);
	if (!ret)
		ret = rdn_append(subject, sizeof(subject), &pos, "O", params->o);
	if (!ret)
		ret = rdn_append(subject, sizeof(subject), &pos, "OU", params->ou);
	if (!ret)
		ret = rdn_append(subject, sizeof(subject), &pos, "CN", params->cn);
	if (!ret)
		ret = rdn_append(subject, sizeof(subject), &pos, "emailAddress", params->email);
	if (ret) {
		ret = fail(-EINVAL, "Subject is too long");
		goto out_unlock;
	}

	/* The alternative names, or the current names if there are none */
	if (params->alt_count) {
		for (size_t i = 0; i < params->alt_count; i++) {
			if (count == SAN_MAX) {
				ret = fail(-E2BIG, "Too many AlternativeNames, at most %d", SAN_MAX);
				goto out_unlock;
			}
			if (san_entry_parse(params->alt_names[i], strlen(params->alt_names[i]),
					    &csr_sans[count])) {
				ret = fail(-EINVAL, "AlternativeNames: \"%s\" is not a DNS name or IPv4 address",
					   params->alt_names[i]);
				goto out_unlock;
			}
			count++;
		}
	} else {
		names_build();
		memcpy(csr_sans, sans, san_count * sizeof(sans[0]));
		count = san_count;
	}
	for (size_t i = 0; i < count; i++) {
		if (csr_sans[i].type == MBEDTLS_X509_SAN_IP_ADDRESS)
			csr_sans[i].p = csr_sans[i].ip4;
	}

	/* CAs generally require the CommonName to be one of the names too */
	if (!san_entry_parse(params->cn, strlen(params->cn), &cn_entry)) {
		for (size_t i = 0; i < count; i++)
			have_cn |= san_entry_eq(&csr_sans[i], cn_entry.type, cn_entry.p, cn_entry.len);
		if (!have_cn) {
			csr_sans[count] = cn_entry;
			if (cn_entry.type == MBEDTLS_X509_SAN_IP_ADDRESS)
				csr_sans[count].p = csr_sans[count].ip4;
			count++;
		}
	}

	ret = key_load(&pk);
	if (ret) {
		ret = fail(ret, "Could not load the HTTPS key");
		goto out_unlock;
	}

	mbedtls_x509write_csr_init(&csr);
	mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
	mbedtls_x509write_csr_set_key(&csr, &pk);

	ret = mbedtls_x509write_csr_set_subject_name(&csr, subject);
	if (ret) {
		ret = fail(-EINVAL, "Invalid subject (err=-0x%04x)", -ret);
		goto out;
	}

	ret = mbedtls_x509write_csr_set_key_usage(&csr, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
	if (ret)
		goto out_err;

	/* Written in reverse order, so link the list backwards */
	memset(san_list, 0, sizeof(san_list));
	for (size_t i = 0; i < count; i++) {
		san_list[i].node.type = csr_sans[i].type;
		san_list[i].node.san.unstructured_name.p = (unsigned char *)csr_sans[i].p;
		san_list[i].node.san.unstructured_name.len = csr_sans[i].len;
		san_list[i].next = i ? &san_list[i - 1] : NULL;
	}
	if (count) {
		ret = mbedtls_x509write_csr_set_subject_alternative_name(&csr, &san_list[count - 1]);
		if (ret)
			goto out_err;
	}

	/* cert_next isn't in use, so use it as scratch */
	ret = mbedtls_x509write_csr_der(&csr, cert_next, CERT_BUF_MAX);
	if (ret < 0)
		goto out_err;

	pos = 0;
	pem[0] = '\0';
	ret = pem_append(pem, size, &pos, "CERTIFICATE REQUEST",
			 cert_next + CERT_BUF_MAX - ret, ret);
	if (ret)
		ret = fail(ret, "CSR is too large");
	goto out;

out_err:
	ret = fail(-EIO, "Could not create the CSR (err=-0x%04x)", -ret);
out:
	mbedtls_x509write_csr_free(&csr);
	mbedtls_pk_free(&pk);
out_unlock:
	k_mutex_unlock(&https_cert_lock);
	return ret;
}

int https_cert_install(char *pem, const char **err)
{
	/* DER of the leaf and chain, at most what fits in a TLS record */
	static uint8_t ders[CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN];
	mbedtls_x509_crt *crt = &scratch_crt;
	mbedtls_x509_crt *c;
	size_t offs[CHAIN_MAX], lens[CHAIN_MAX];
	size_t n = 0, used = 0, len, pos;
	uint8_t spki[SPKI_DER_MAX];
	size_t spki_len;
	mbedtls_pk_context pk;
	char *p = pem;
	int ret;

	*err = err_msg;

	k_mutex_lock(&https_cert_lock, K_FOREVER);
	mbedtls_x509_crt_init(crt);

	if (!key_len) {
		ret = fail(-ENOENT, "No HTTPS key");
		goto out;
	}

	for (;;) {
		ret = pem_next_cert(&p, ders + used, sizeof(ders) - used, &len);
		if (ret == -ENOENT)
			break;
		if (ret == -ENOMEM) {
			ret = fail(-E2BIG, "Certificate chain is too large, at most %zu bytes of DER",
				   sizeof(ders));
			goto out;
		}
		if (ret) {
			ret = fail(-EINVAL, "Certificate %zu is not valid PEM", n + 1);
			goto out;
		}
		if (n == CHAIN_MAX) {
			ret = fail(-E2BIG, "Too many certificates, at most %d including the chain",
				   CHAIN_MAX);
			goto out;
		}
		offs[n] = used;
		lens[n] = len;
		used += len;
		n++;
	}

	if (!n) {
		ret = fail(-EINVAL, "No PEM certificate found");
		goto out;
	}

	/* The leaf has to fit in the buffers it's shown through */
	if (lens[0] > HTTPS_CERT_LEAF_DER_MAX) {
		ret = fail(-E2BIG, "Certificate is too large (%zu bytes), at most %d bytes of DER",
			   lens[0], HTTPS_CERT_LEAF_DER_MAX);
		goto out;
	}

	/*
	 * The server sends the leaf and chain in one TLS record: each
	 * certificate has a 3 byte length and, in TLS 1.3, 2 bytes of
	 * extensions, plus the message header.
	 */
	if (used + 5 * n + 8 > CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN) {
		ret = fail(-E2BIG, "Certificate chain is too large (%zu bytes), it must fit in %d bytes",
			   used + 5 * n + 8, CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN);
		goto out;
	}

	for (size_t i = 0; i < n; i++) {
		ret = mbedtls_x509_crt_parse_der(crt, ders + offs[i], lens[i]);
		if (ret) {
			ret = fail(-EINVAL, "Certificate %zu could not be parsed (err=-0x%04x), only "
				   "ECDSA P-256 keys and ECDSA signatures are supported", i + 1, -ret);
			goto out;
		}
	}

	ret = key_load(&pk);
	if (ret) {
		ret = fail(ret, "Could not load the HTTPS key");
		goto out;
	}
	ret = key_spki(&pk, spki, &spki_len);
	mbedtls_pk_free(&pk);
	if (ret) {
		ret = fail(ret, "Could not load the HTTPS key");
		goto out;
	}

	if (crt->pk_raw.len != spki_len || memcmp(crt->pk_raw.p, spki, spki_len)) {
		ret = fail(-EINVAL, "Certificate is not for this BMC's key, it must be issued "
			   "from a CSR made with GenerateCSR");
		goto out;
	}

	/* Each certificate in the chain must be the issuer of the one before */
	n = 1;
	for (c = crt; c->next; c = c->next, n++) {
		if (c->issuer_raw.len != c->next->subject_raw.len ||
		    memcmp(c->issuer_raw.p, c->next->subject_raw.p, c->issuer_raw.len)) {
			ret = fail(-EINVAL, "Certificate %zu is not the issuer of certificate %zu",
				   n + 1, n);
			goto out;
		}
	}

	/*
	 * And must have signed it. The last certificate is the trust anchor,
	 * so isn't checked itself. Validity dates aren't checked, there's no
	 * MBEDTLS_HAVE_TIME_DATE.
	 */
	if (n > 1) {
		uint32_t flags;

		ret = mbedtls_x509_crt_verify(crt, c, NULL, NULL, &flags, NULL, NULL);
		if (ret) {
			char why[96];

			mbedtls_x509_crt_verify_info(why, sizeof(why), "", flags);
			why[strcspn(why, "\n")] = '\0';
			ret = fail(-EINVAL, "Certificate chain does not verify: %s", why);
			goto out;
		}
	}

	if (n == 1) {
		/* A single certificate is registered as DER */
		memcpy(cert_next, ders, lens[0]);
		len = lens[0];
	} else {
		/* A chain is registered as NUL terminated PEM */
		pos = 0;
		cert_next[0] = '\0';
		for (size_t i = 0; i < n; i++) {
			ret = pem_append((char *)cert_next, CERT_BUF_MAX, &pos, "CERTIFICATE",
					 ders + offs[i], lens[i]);
			if (ret) {
				ret = fail(-E2BIG, "Certificate chain is too large");
				goto out;
			}
		}
		len = pos + 1;
	}

	/* info_update() below uses scratch_crt too */
	mbedtls_x509_crt_free(crt);

	ret = cert_switch(len, HTTPS_CERT_INSTALLED);
	if (ret) {
		ret = fail(ret, "Could not register the certificate");
		goto out;
	}

	save(CONFIG_BLOB_HTTPS_CERT, cert_der, cert_len);
	save(CONFIG_BLOB_HTTPS_CERT_SOURCE, &cert_source, sizeof(cert_source));

	names_build();
	info_update(spki, spki_len);
	info_log();
	ret = 0;

out:
	mbedtls_x509_crt_free(crt);
	k_mutex_unlock(&https_cert_lock);
	return ret;
}

int https_cert_self_signed(void)
{
	int ret;

	k_mutex_lock(&https_cert_lock, K_FOREVER);
	if (!key_len) {
		ret = -ENOENT;
	} else {
		names_build();
		ret = cert_reissue();
	}
	k_mutex_unlock(&https_cert_lock);

	return ret;
}

int https_cert_cmd_self_signed(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!is_boot_finished()) {
		shell_error(sh, "must wait for boot to finish");
		return -EAGAIN;
	}

	ret = https_cert_self_signed();
	if (ret) {
		shell_error(sh, "Could not issue a self-signed certificate (err=%d)", ret);
		return ret;
	}

	return https_cert_cmd_show(sh, 0, NULL);
}
