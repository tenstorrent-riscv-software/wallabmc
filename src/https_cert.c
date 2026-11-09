/*
 * HTTPS server key and certificate
 *
 * Each board generates its own P-256 key on first boot and keeps it in
 * config. The certificate is self-signed for the BMC's hostname and static
 * IPv4 address, and is reissued with the same key when they change.
 *
 * SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/byteorder.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <psa/crypto.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/oid.h>
#include <mbedtls/asn1.h>
#include <mbedtls/platform_util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wallabmc_https_cert, LOG_LEVEL_INF);

#include "https_cert.h"
#include "config.h"
#include "main.h"

/* SEC1 ECPrivateKey DER for P-256 is 121 bytes */
#define KEY_DER_MAX		128
/* SubjectPublicKeyInfo DER for P-256 is 91 bytes */
#define SPKI_DER_MAX		96
/* Uncompressed P-256 point, the end of the SPKI */
#define EC_POINT_LEN		65
/* Self-signed leaf is about 500 bytes, allow for installed certs */
#define CERT_DER_MAX		1536

#define SHA256_LEN		32
#define SHA1_LEN		20

#define CERT_NOT_BEFORE		"20000101000000"
#define CERT_NOT_AFTER		"99991231235959"
#define CERT_ORG		"WallaBMC"

static K_MUTEX_DEFINE(https_cert_lock);

/* Registered as the TLS private key credential */
static uint8_t key_der[KEY_DER_MAX];
static size_t key_len;

/* Registered as the TLS certificate credential */
static uint8_t cert_der[CERT_DER_MAX];
static size_t cert_len;
static uint8_t cert_source;

/* Scratch, used under https_cert_lock to keep large objects off the stack */
static uint8_t scratch[CERT_DER_MAX];
static mbedtls_x509_crt scratch_crt;

/* Cached for display, so showing the cert needs no crypto */
static struct {
	bool valid;
	char fingerprint[SHA256_LEN * 3];	/* AA:BB:..., NUL terminated */
	char pin[(SHA256_LEN + 2) / 3 * 4 + 1];
	char subject[96];
	char names[160];
} info;

/* The names a self-signed certificate is issued for */
struct cert_names {
	const char *hostname;
	uint32_t ip4;		/* network byte order, 0 for none */
};

static void cert_names_current(struct cert_names *names)
{
	names->hostname = config_bmc_hostname();
	names->ip4 = config_bmc_default_ip4();
}

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
		ret = mbedtls_pk_write_key_der(&pk, scratch, sizeof(scratch));
	mbedtls_pk_free(&pk);
	psa_destroy_key(id);

	if (ret < 0) {
		LOG_ERR("Key export failed (err=-0x%04x)", -ret);
		mbedtls_platform_zeroize(scratch, sizeof(scratch));
		return -EIO;
	}
	if (ret > sizeof(key_der)) {
		LOG_ERR("Key too large (%d bytes)", ret);
		mbedtls_platform_zeroize(scratch, sizeof(scratch));
		return -ENOMEM;
	}

	/* The DER is written at the end of the buffer */
	memcpy(key_der, scratch + sizeof(scratch) - ret, ret);
	key_len = ret;
	mbedtls_platform_zeroize(scratch, sizeof(scratch));

	return 0;
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

static int cert_issue(mbedtls_pk_context *pk, const uint8_t *spki, size_t spki_len,
		      const struct cert_names *names)
{
	mbedtls_x509write_cert crt;
	mbedtls_x509_san_list san[2];
	mbedtls_asn1_sequence eku;
	uint8_t serial[16];
	uint8_t ski[2 + SHA1_LEN];
	uint8_t ip4[4];
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

	ret = snprintf(subject, sizeof(subject), "CN=%s,O=" CERT_ORG, names->hostname);
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

	memset(san, 0, sizeof(san));
	san[0].node.type = MBEDTLS_X509_SAN_DNS_NAME;
	san[0].node.san.unstructured_name.p = (unsigned char *)names->hostname;
	san[0].node.san.unstructured_name.len = strlen(names->hostname);
	if (names->ip4) {
		memcpy(ip4, &names->ip4, sizeof(ip4));
		san[0].next = &san[1];
		san[1].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
		san[1].node.san.unstructured_name.p = ip4;
		san[1].node.san.unstructured_name.len = sizeof(ip4);
	}
	ret = mbedtls_x509write_crt_set_subject_alternative_name(&crt, san);
	if (ret)
		goto out;

	ret = mbedtls_x509write_crt_der(&crt, scratch, sizeof(scratch));
	if (ret < 0)
		goto out;
	if (ret > sizeof(cert_der)) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy(cert_der, scratch + sizeof(scratch) - ret, ret);
	cert_len = ret;
	cert_source = HTTPS_CERT_SELF_SIGNED;
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

/* True if the certificate's SANs are exactly the given names */
static bool cert_names_equal(const mbedtls_x509_crt *crt, const struct cert_names *names)
{
	const mbedtls_x509_sequence *cur;
	bool have_dns = false, have_ip4 = false;

	for (cur = &crt->subject_alt_names; cur && cur->buf.p; cur = cur->next) {
		const mbedtls_x509_buf *san = &cur->buf;

		if (san_is(san, MBEDTLS_X509_SAN_DNS_NAME) && !have_dns &&
		    san->len == strlen(names->hostname) &&
		    !strncasecmp((const char *)san->p, names->hostname, san->len)) {
			have_dns = true;
		} else if (san_is(san, MBEDTLS_X509_SAN_IP_ADDRESS) && !have_ip4 &&
			   names->ip4 && san->len == 4 &&
			   !memcmp(san->p, &names->ip4, 4)) {
			have_ip4 = true;
		} else {
			return false;
		}
	}

	return have_dns && have_ip4 == !!names->ip4;
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
		snprintf(buf + len, size - len, "%s(type %d)", sep,
			 san->tag & ~MBEDTLS_ASN1_CONTEXT_SPECIFIC);
	}
}

static int cert_parse(mbedtls_x509_crt *crt, const uint8_t *der, size_t len)
{
	int ret;

	mbedtls_x509_crt_init(crt);
	ret = mbedtls_x509_crt_parse_der_nocopy(crt, der, len);
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

	if (sha_compute(PSA_ALG_SHA_256, cert_der, cert_len, hash, sizeof(hash)))
		return;
	for (int i = 0; i < sizeof(hash); i++)
		sprintf(info.fingerprint + i * 3 - (i ? 1 : 0), i ? ":%02X" : "%02X", hash[i]);

	/* Public key pin, as used by curl --pinnedpubkey sha256//<pin> */
	if (sha_compute(PSA_ALG_SHA_256, spki, spki_len, hash, sizeof(hash)))
		return;
	ret = base64_encode((uint8_t *)info.pin, sizeof(info.pin), &olen, hash, sizeof(hash));
	if (ret)
		return;

	if (cert_parse(crt, cert_der, cert_len))
		return;

	ret = mbedtls_x509_dn_gets(info.subject, sizeof(info.subject), &crt->subject);
	if (ret < 0)
		strcpy(info.subject, "?");

	for (cur = &crt->subject_alt_names; cur && cur->buf.p; cur = cur->next)
		names_append(info.names, sizeof(info.names), &cur->buf);

	mbedtls_x509_crt_free(crt);
	info.valid = true;
}

static void info_log(void)
{
	if (!info.valid)
		return;

	LOG_INF("HTTPS certificate (%s): %s",
		cert_source == HTTPS_CERT_INSTALLED ? "installed" : "self-signed",
		info.subject);
	LOG_INF("  Names: %s", info.names);
	LOG_INF("  SHA-256 fingerprint: %s", info.fingerprint);
	LOG_INF("  Public key pin: sha256//%s", info.pin);
}

/*
 * Check a stored certificate can be used with the key. A self-signed
 * certificate must also be for the current names, otherwise it's reissued.
 */
static bool cert_usable(const uint8_t *spki, size_t spki_len, const struct cert_names *names)
{
	mbedtls_x509_crt *crt = &scratch_crt;
	bool ok = true;

	if (cert_parse(crt, cert_der, cert_len))
		return false;

	if (crt->pk_raw.len != spki_len || memcmp(crt->pk_raw.p, spki, spki_len)) {
		LOG_WRN("Stored certificate is not for the stored key");
		ok = false;
	} else if (cert_source == HTTPS_CERT_SELF_SIGNED && !cert_names_equal(crt, names)) {
		LOG_INF("Hostname or IP address changed, reissuing certificate");
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

static int https_cert_load(void)
{
	mbedtls_pk_context pk;
	struct cert_names names;
	uint8_t spki[SPKI_DER_MAX];
	size_t spki_len;
	bool new_key = false;
	uint32_t t;
	ssize_t rc;
	int ret;

	if (!config_persistent())
		LOG_WRN("No persistent storage, the HTTPS key will change on every boot");

	rc = config_blob_read(CONFIG_BLOB_HTTPS_KEY, key_der, sizeof(key_der));
	key_len = rc > 0 ? rc : 0;
	if (key_len) {
		ret = key_load(&pk);
		if (ret) {
			LOG_ERR("Stored HTTPS key is invalid, generating a new one");
			key_len = 0;
		}
	} else if (rc != -ENOENT && rc != -ENODEV) {
		LOG_ERR("Could not read HTTPS key (err=%zd), generating a new one", rc);
	}

	if (!key_len) {
		LOG_INF("Generating HTTPS key");
		t = k_uptime_get_32();
		ret = key_generate();
		if (ret)
			return ret;
		LOG_INF("Generated HTTPS key in %u ms", k_uptime_get_32() - t);
		new_key = true;
		save(CONFIG_BLOB_HTTPS_KEY, key_der, key_len);
		ret = key_load(&pk);
		if (ret)
			return ret;
	}

	ret = key_spki(&pk, spki, &spki_len);
	if (ret)
		goto out;

	cert_names_current(&names);
	cert_len = 0;
	if (!new_key) {
		rc = config_blob_read(CONFIG_BLOB_HTTPS_CERT, cert_der, sizeof(cert_der));
		if (rc > 0) {
			cert_len = rc;
			if (config_blob_read(CONFIG_BLOB_HTTPS_CERT_SOURCE, &cert_source,
					     sizeof(cert_source)) != sizeof(cert_source))
				cert_source = HTTPS_CERT_SELF_SIGNED;
			if (!cert_usable(spki, spki_len, &names))
				cert_len = 0;
		}
	}

	if (!cert_len) {
		t = k_uptime_get_32();
		ret = cert_issue(&pk, spki, spki_len, &names);
		if (ret)
			goto out;
		LOG_INF("Issued self-signed HTTPS certificate in %u ms",
			k_uptime_get_32() - t);
		save(CONFIG_BLOB_HTTPS_CERT, cert_der, cert_len);
		save(CONFIG_BLOB_HTTPS_CERT_SOURCE, &cert_source, sizeof(cert_source));
	}

	info_update(spki, spki_len);
	info_log();

out:
	mbedtls_pk_free(&pk);
	if (ret)
		cert_len = 0;
	return ret;
}

int https_cert_init(void)
{
	int ret;

	k_mutex_lock(&https_cert_lock, K_FOREVER);
	ret = https_cert_load();
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

int https_cert_get(const uint8_t **cert, size_t *cert_len_out,
		   const uint8_t **key, size_t *key_len_out)
{
	int ret = -ENOENT;

	k_mutex_lock(&https_cert_lock, K_FOREVER);
	if (cert_len && key_len) {
		*cert = cert_der;
		*cert_len_out = cert_len;
		*key = key_der;
		*key_len_out = key_len;
		ret = 0;
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
	shell_print(sh, "Names:               %s", info.names);
	shell_print(sh, "SHA-256 fingerprint: %s", info.fingerprint);
	shell_print(sh, "Public key pin:      sha256//%s", info.pin);
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
	 * Generating a key here would use mbedTLS concurrently with the TLS
	 * connections, which isn't thread safe. Delete the stored key so a
	 * new one is generated on the next boot, before the HTTP server
	 * starts.
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
