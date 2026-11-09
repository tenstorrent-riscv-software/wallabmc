#!/bin/bash
# SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Test installing a CA-signed HTTPS certificate through Redfish, against a
# live device (or QEMU):
#
#  1. Make a throwaway root and intermediate CA with openssl.
#  2. Ask the BMC for a CSR (CertificateService.GenerateCSR).
#  3. Sign it with the intermediate and install the certificate and chain
#     (CertificateService.ReplaceCertificate).
#  4. Check the BMC serves the new certificate, that it verifies against the
#     test root CA, and that the key didn't change.
#  5. Check bad certificates are rejected and leave the certificate alone.
#     This includes ones whose key usage doesn't allow HTTPS, and one whose
#     subject and issuer are too long to show in Redfish.
#  6. Change the hostname, which must keep the installed certificate.
#  7. DELETE the certificate, which puts back a self-signed one, for the new
#     hostname, and change the hostname back, which reissues it.
#
# THIS IS DESTRUCTIVE: the BMC's current HTTPS certificate is replaced. The
# key is kept, so clients that pin the key are unaffected. The BMC is left
# with a new self-signed certificate (or the test certificate with --keep),
# so its fingerprint changes. If the current certificate is self-signed
# that's all, and the test just runs. If it isn't, it's lost and must be
# installed again, so the test asks for confirmation first. The hostname is
# changed during the test and changed back at the end, even on failure.
#
# Usage: test-https-cert.sh [--yes] [--keep] [IP]
#
# Needs curl, jq and OpenSSL 3 (for -copy_extensions).

set -u -o pipefail

BMC_USER="${BMC_USER:-admin}"
BMC_PASS="${BMC_PASS:-admin}"

CERTS_PATH=/redfish/v1/Managers/bmc/NetworkProtocol/HTTPS/Certificates
CERT_PATH=$CERTS_PATH/1
SERVICE_PATH=/redfish/v1/CertificateService
ETH_PATH=/redfish/v1/Managers/bmc/EthernetInterfaces/eth0

usage() {
	cat <<'EOF'
Usage: test-https-cert.sh [--yes] [--keep] [IP]
  --yes   Don't ask for confirmation when the current certificate isn't
          self-signed (needed when not run from a terminal)
  --keep  Leave the test certificate installed, and keep the test CA files
  IP      Target device address (default: 192.168.1.110)

DESTRUCTIVE: replaces the BMC's HTTPS certificate, and asks first if it
isn't self-signed. Set BMC_USER and BMC_PASS for credentials other than
admin/admin.
EOF
}

yes=0
keep=0
IP=""
for arg in "$@"; do
	case "$arg" in
	--yes) yes=1 ;;
	--keep) keep=1 ;;
	-h|--help) usage; exit 0 ;;
	-*) echo "Unknown option: $arg" >&2; usage >&2; exit 1 ;;
	*) IP="$arg" ;;
	esac
done
IP="${IP:-192.168.1.110}"
BASE="https://$IP"

for tool in curl jq openssl; do
	if ! command -v "$tool" >/dev/null; then
		echo "$tool is required" >&2
		exit 1
	fi
done
if ! openssl x509 -help 2>&1 | grep -q copy_extensions; then
	echo "OpenSSL 3 is required (for -copy_extensions)" >&2
	exit 1
fi

pass=0
installed=0
host=""

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

ok() {
	echo "PASS: $*"
	pass=$((pass + 1))
}

# rf METHOD PATH [JSON]: Redfish request, the body is in $rf_body and the
# status code in $code. If curl fails the test fails, unless rf_fatal=0,
# when $code is 000.
rf_body=""
rf_fatal=1
rf() {
	local out rc

	out=$(curl -sk -m 30 -u "$BMC_USER:$BMC_PASS" -X "$1" \
		-H 'Content-Type: application/json' ${3:+--data-binary "$3"} \
		-w '\n%{http_code}' "$BASE$2")
	rc=$?
	if [ $rc != 0 ]; then
		[ $rf_fatal = 0 ] || fail "$1 $2: curl failed (rc=$rc)"
		echo "WARNING: $1 $2: curl failed (rc=$rc)" >&2
		code=000
		rf_body=""
		return
	fi
	code="${out##*$'\n'}"
	rf_body="${out%$'\n'*}"
}

# Fingerprint (AA:BB:...) of the certificate the BMC serves
served_fingerprint() {
	openssl s_client -connect "$IP:443" </dev/null 2>/dev/null |
		openssl x509 -noout -fingerprint -sha256 | sed 's/^.*=//'
}

# Base64 SHA-256 of the SubjectPublicKeyInfo, as used for key pinning
pin_of_server() {
	openssl s_client -connect "$IP:443" </dev/null 2>/dev/null |
		openssl x509 -noout -pubkey | openssl pkey -pubin -outform der |
		openssl dgst -sha256 -binary | base64
}

pin_of_csr() {
	openssl req -in "$1" -noout -pubkey | openssl pkey -pubin -outform der |
		openssl dgst -sha256 -binary | base64
}

# Names (DNS:..., IP Address:...) of the certificate the BMC serves, one per line
served_names() {
	openssl s_client -connect "$IP:443" </dev/null 2>/dev/null |
		openssl x509 -noout -ext subjectAltName | tail -n +2 |
		tr ',' '\n' | sed 's/^ *//'
}

get_hostname() {
	rf GET "$ETH_PATH"
	[ "$code" = 200 ] || fail "GET $ETH_PATH: HTTP $code"
	jq -r .HostName <<<"$rf_body"
}

# Set the hostname with a PATCH, and check it took
set_hostname() {
	rf PATCH "$ETH_PATH" "$(jq -n --arg h "$1" '{HostName: $h}')"
	[ "$code" = 200 ] || [ "$code" = 204 ] || fail "PATCH HostName $1: HTTP $code: $rf_body"
	[ "$(get_hostname)" = "$1" ] || fail "HostName is not $1 after PATCH"
}

# Install PEM file $1 with ReplaceCertificate
replace_certificate() {
	local body

	body=$(jq -n --rawfile pem "$1" --arg uri "$CERT_PATH" \
		'{CertificateString: $pem, CertificateType: "PEM",
		  CertificateUri: {"@odata.id": $uri}}')
	rf POST "$SERVICE_PATH/Actions/CertificateService.ReplaceCertificate" "$body"
}

# The curve of the test CAs' keys, and the hash they sign with
ca_curve=prime256v1
ca_md=sha256

# Sign CSR $1 with CA $2 (cert + key prefix), output $3, using the alt names
# in the CSR, and the extensions in $4 (default leaf.ext). Any more arguments
# are passed to openssl.
sign_csr() {
	local csr=$1 ca=$2 out=$3 ext=${4:-$tmp/leaf.ext}

	shift $(($# < 4 ? $# : 4))
	openssl x509 -req -in "$csr" -CA "$ca.pem" -CAkey "$ca.key" -CAcreateserial \
		-days 30 -"$ca_md" -copy_extensions copyall -extfile "$ext" "$@" \
		-out "$out" 2>/dev/null || fail "signing $csr failed"
}

# The value of the attribute $1 (eg. organizationName) in the subject of
# the PEM CSR or certificate on stdin, unescaped
pem_subject_attr() {
	openssl asn1parse 2>/dev/null | grep -A1 ":$1\$" | sed -n 's/^.*STRING *://p' | head -n 1
}

# Is the BMC reachable, and what's there now?
rf GET "$CERT_PATH"
[ "$code" = 200 ] || fail "GET $CERT_PATH: HTTP $code (wrong IP or credentials, or no HTTPS certificate support?)"
orig_cert="$rf_body"
orig_fp=$(jq -r .Fingerprint <<<"$orig_cert")

tmp=$(mktemp -d "${TMPDIR:-/tmp}/wallabmc-cert-test.XXXXXX")
jq -r .CertificateString <<<"$orig_cert" >"$tmp/original.pem"

# Put things back as well as it can, carrying on if a request fails
cleanup() {
	local rc=$?

	rf_fatal=0
	if [ $installed = 1 ] && [ $keep = 0 ]; then
		echo "Putting back a self-signed certificate"
		rf DELETE "$CERT_PATH"
		[ "$code" = 204 ] || echo "WARNING: DELETE $CERT_PATH: HTTP $code, the test certificate is still installed" >&2
	fi
	if [ -n "$host" ]; then
		rf GET "$ETH_PATH"
		if [ "$(jq -r .HostName <<<"$rf_body")" != "$host" ]; then
			echo "Putting back the hostname $host"
			rf PATCH "$ETH_PATH" "$(jq -n --arg h "$host" '{HostName: $h}')"
			[ "$code" = 200 ] || [ "$code" = 204 ] ||
				echo "WARNING: PATCH HostName: HTTP $code, the hostname wasn't put back" >&2
		fi
	fi
	if [ $keep = 1 ] || [ $rc != 0 ]; then
		echo "Test files are in $tmp (original.pem is the certificate before the test)"
	else
		rm -rf "$tmp"
	fi
}
trap cleanup EXIT

# Is the current certificate signed by its own key? Then it's one the BMC
# issued itself, and the DELETE at the end issues an equivalent one, so
# nothing is lost but the exact certificate. Anything else was installed,
# and can only be put back by whoever installed it.
if openssl verify -check_ss_sig -no_check_time -CAfile "$tmp/original.pem" \
		"$tmp/original.pem" >/dev/null 2>&1; then
	if [ $keep = 1 ]; then
		echo "The current certificate ($orig_fp) is self-signed, it will be replaced by the test certificate (--keep)."
	else
		echo "The current certificate ($orig_fp) is self-signed, it will be replaced by a new self-signed one, with the same key."
	fi
else
	cat >&2 <<EOF

########################################################################
#  WARNING: this test REPLACES the HTTPS certificate on $IP.
#
#  The current certificate is NOT self-signed:
#    Subject:     $(jq -c .Subject <<<"$orig_cert")
#    Issuer:      $(jq -c .Issuer <<<"$orig_cert")
#    Fingerprint: $orig_fp
#
#  It will be lost, and must be installed again, with its chain,
#  afterwards. The key is kept.
########################################################################

EOF
	if [ $yes = 0 ]; then
		if [ ! -t 0 ]; then
			echo "Not running from a terminal, pass --yes to confirm." >&2
			exit 1
		fi
		read -r -p "Type the BMC address ($IP) to continue: " answer
		[ "$answer" = "$IP" ] || { echo "Aborted." >&2; exit 1; }
	fi
fi

orig_pin=$(pin_of_server)
[ -n "$orig_pin" ] || fail "couldn't get the key from the TLS handshake"
echo "Key pin: sha256//$orig_pin"

# Throwaway CAs: a root, an intermediate signed by it, and an impostor with
# the intermediate's name but its own key, for a broken chain. ECDSA P-256
# keeps the chain small, the request must fit in a 4 KiB TLS record.
make_ca() {	# name subject [issuer]
	openssl ecparam -name "$ca_curve" -genkey -noout -out "$tmp/$1.key" 2>/dev/null
	if [ $# = 2 ]; then
		openssl req -x509 -new -key "$tmp/$1.key" -subj "$2" -days 30 -"$ca_md" \
			-addext basicConstraints=critical,CA:TRUE \
			-addext keyUsage=critical,keyCertSign,cRLSign \
			-out "$tmp/$1.pem" 2>/dev/null
	else
		openssl req -new -key "$tmp/$1.key" -subj "$2" -out "$tmp/$1.csr" 2>/dev/null
		printf '%s\n' "basicConstraints=critical,CA:TRUE,pathlen:0" \
			"keyUsage=critical,keyCertSign,cRLSign" \
			"subjectKeyIdentifier=hash" \
			"authorityKeyIdentifier=keyid" >"$tmp/ca.ext"
		openssl x509 -req -in "$tmp/$1.csr" -CA "$tmp/$3.pem" -CAkey "$tmp/$3.key" \
			-CAcreateserial -days 30 -"$ca_md" -extfile "$tmp/ca.ext" \
			-out "$tmp/$1.pem" 2>/dev/null
	fi || fail "making test CA $1 failed"
}
make_ca root "/O=WallaBMC Test/CN=WallaBMC Test Root CA"
make_ca int "/O=WallaBMC Test/CN=WallaBMC Test Intermediate CA" root
make_ca other "/O=WallaBMC Test/CN=WallaBMC Test Intermediate CA"
# A CA with the longest names the BMC shows, all quotes, which are escaped
# in JSON, so they're twice as long in the Certificate resource
quotes=$(printf '"%.0s' $(seq 64))
long_dn="/C=US/ST=$quotes/L=$quotes/O=$quotes/OU=$quotes/CN=$quotes/emailAddress=$quotes"
make_ca long "$long_dn"
# P-384 CAs signing with SHA-384, like Let's Encrypt's ECDSA chain, which
# are supported, and P-521 ones, which aren't. They also sign with SHA-384,
# so it's only the P-521 key that isn't supported.
ca_curve=secp384r1 ca_md=sha384
make_ca root384 "/O=WallaBMC Test/CN=WallaBMC Test P-384 Root CA"
make_ca int384 "/O=WallaBMC Test/CN=WallaBMC Test P-384 Intermediate CA" root384
ca_curve=secp521r1 ca_md=sha384
make_ca root521 "/O=WallaBMC Test/CN=WallaBMC Test P-521 Root CA"
make_ca int521 "/O=WallaBMC Test/CN=WallaBMC Test P-521 Intermediate CA" root521
ca_curve=prime256v1 ca_md=sha256
printf '%s\n' "basicConstraints=critical,CA:FALSE" \
	"keyUsage=critical,digitalSignature" \
	"extendedKeyUsage=serverAuth" \
	"subjectKeyIdentifier=hash" \
	"authorityKeyIdentifier=keyid" >"$tmp/leaf.ext"
# No key usage or extended key usage, which allows any use
printf '%s\n' "basicConstraints=critical,CA:FALSE" \
	"subjectKeyIdentifier=hash" \
	"authorityKeyIdentifier=keyid" >"$tmp/leaf-any.ext"
# Key usage or extended key usage that doesn't allow HTTPS
sed 's/^keyUsage=.*/keyUsage=critical,keyAgreement/' "$tmp/leaf.ext" >"$tmp/leaf-ka.ext"
sed 's/^extendedKeyUsage=.*/extendedKeyUsage=clientAuth/' "$tmp/leaf.ext" >"$tmp/leaf-client.ext"
# Padded so the PEM and the longest names don't fit in the Certificate
# resource together, though the certificate is under 2 KiB
cp "$tmp/leaf.ext" "$tmp/leaf-long.ext"
echo "nsComment=$(printf 'x%.0s' $(seq 400))" >>"$tmp/leaf-long.ext"
ok "made test CAs"

# GenerateCSR, for the hostname, a made up FQDN and the IP address. The
# organization has characters that are escaped in JSON and in the subject,
# and ends with a backslash, which is followed by a comma in the subject.
host=$(get_hostname)
fqdn="$host.wallabmc-test.example"
org='WallaBMC "Test", A\B\'
csr_req=$(jq -n --arg cn "$fqdn" --arg host "$host" --arg ip "$IP" \
	--arg certs "$CERTS_PATH" --arg org "$org" \
	'{CertificateCollection: {"@odata.id": $certs}, CommonName: $cn,
	  Organization: $org, Country: "US",
	  AlternativeNames: [$cn, $host, $ip],
	  KeyPairAlgorithm: "TPM_ALG_ECDSA", KeyCurveId: "TPM_ECC_NIST_P256"}')
rf POST "$SERVICE_PATH/Actions/CertificateService.GenerateCSR" "$csr_req"
[ "$code" = 200 ] || fail "GenerateCSR: HTTP $code: $rf_body"
jq -r .CSRString <<<"$rf_body" >"$tmp/bmc.csr"
openssl req -in "$tmp/bmc.csr" -noout -verify 2>/dev/null || fail "CSR signature doesn't verify"
[ "$(pin_of_csr "$tmp/bmc.csr")" = "$orig_pin" ] || fail "CSR is for a different key than the server's"
openssl req -in "$tmp/bmc.csr" -noout -text | grep -q "DNS:$fqdn" || fail "CSR is missing DNS:$fqdn"
openssl req -in "$tmp/bmc.csr" -noout -text | grep -q "IP Address:$IP" || fail "CSR is missing IP:$IP"
ok "GenerateCSR made a CSR for the server's key with the requested names"

csr_org=$(pem_subject_attr organizationName <"$tmp/bmc.csr")
[ "$csr_org" = "$org" ] || fail "CSR Organization is '$csr_org', not '$org'"
ok "GenerateCSR unescapes the JSON strings: Organization is '$org'"

# Escaped slashes, as some JSON encoders send, are fine
rf POST "$SERVICE_PATH/Actions/CertificateService.GenerateCSR" "${csr_req//\//\\/}"
[ "$code" = 200 ] || fail "GenerateCSR with escaped slashes: HTTP $code: $rf_body"
ok "GenerateCSR accepts escaped slashes"

# \u escapes aren't supported, and are rejected rather than used as they are
rf POST "$SERVICE_PATH/Actions/CertificateService.GenerateCSR" \
	"$(jq -c '.Organization = "UNICODE"' <<<"$csr_req" | sed 's/UNICODE/\\u0041/')"
[ "$code" = 400 ] || fail "GenerateCSR with a \\u escape: expected HTTP 400, got $code"
ok "GenerateCSR rejects \\u escapes: $(jq -r '.error.message // empty' <<<"$rf_body")"

# GenerateCSR for an unsupported key type must fail
rf POST "$SERVICE_PATH/Actions/CertificateService.GenerateCSR" \
	"$(jq -c '.KeyPairAlgorithm = "TPM_ALG_RSA" | del(.KeyCurveId)' <<<"$csr_req")"
[ "$code" = 400 ] || fail "GenerateCSR for RSA: expected HTTP 400, got $code"
ok "GenerateCSR rejects an RSA key"

# Sign it and make the chain: leaf then intermediate
sign_csr "$tmp/bmc.csr" "$tmp/int" "$tmp/leaf.pem"
cat "$tmp/leaf.pem" "$tmp/int.pem" >"$tmp/chain.pem"
leaf_fp=$(openssl x509 -in "$tmp/leaf.pem" -noout -fingerprint -sha256 | sed 's/^.*=//')

# Bad certificates are rejected, and the certificate is unchanged
before_fp=$(served_fingerprint)
expect_rejected() {	# description file
	replace_certificate "$2"
	[ "$code" = 400 ] || fail "$1: expected HTTP 400, got $code: $rf_body"
	[ "$(served_fingerprint)" = "$before_fp" ] || fail "$1: served certificate changed"
	ok "ReplaceCertificate rejects $1: $(jq -r '.error.message // empty' <<<"$rf_body")"
}

openssl ecparam -name prime256v1 -genkey -noout -out "$tmp/wrong.key" 2>/dev/null
openssl req -new -key "$tmp/wrong.key" -subj "/CN=$fqdn" -out "$tmp/wrong.csr" 2>/dev/null
sign_csr "$tmp/wrong.csr" "$tmp/int" "$tmp/wrong.pem"
expect_rejected "a certificate for another key" "$tmp/wrong.pem"

cat "$tmp/leaf.pem" "$tmp/root.pem" >"$tmp/badchain.pem"
expect_rejected "a chain that doesn't link up" "$tmp/badchain.pem"

cat "$tmp/leaf.pem" "$tmp/other.pem" >"$tmp/badsig.pem"
expect_rejected "a chain whose intermediate has the right name but the wrong key" "$tmp/badsig.pem"

printf -- '-----BEGIN CERTIFICATE-----\nbm90IGEgY2VydGlmaWNhdGU=\n-----END CERTIFICATE-----\n' >"$tmp/junk.pem"
expect_rejected "junk" "$tmp/junk.pem"

ca_md=sha384
sign_csr "$tmp/bmc.csr" "$tmp/int521" "$tmp/leaf521.pem"
ca_md=sha256
cat "$tmp/leaf521.pem" "$tmp/int521.pem" >"$tmp/chain521.pem"
expect_rejected "a chain with a P-521 intermediate" "$tmp/chain521.pem"

sign_csr "$tmp/bmc.csr" "$tmp/int" "$tmp/leaf-ka.pem" "$tmp/leaf-ka.ext"
cat "$tmp/leaf-ka.pem" "$tmp/int.pem" >"$tmp/chain-ka.pem"
expect_rejected "a certificate whose key usage is only keyAgreement" "$tmp/chain-ka.pem"

sign_csr "$tmp/bmc.csr" "$tmp/int" "$tmp/leaf-client.pem" "$tmp/leaf-client.ext"
cat "$tmp/leaf-client.pem" "$tmp/int.pem" >"$tmp/chain-client.pem"
expect_rejected "a certificate whose extended key usage is only clientAuth" "$tmp/chain-client.pem"

# Just the certificate, the chain isn't checked without it
sign_csr "$tmp/bmc.csr" "$tmp/long" "$tmp/leaf-long.pem" "$tmp/leaf-long.ext" -subj "$long_dn"
[ "$(openssl x509 -in "$tmp/leaf-long.pem" -outform der | wc -c)" -le 2048 ] ||
	fail "the certificate with long names is over 2 KiB"
expect_rejected "a certificate whose subject and issuer are too long to show" "$tmp/leaf-long.pem"

# A chain with a P-384 intermediate, signing with SHA-384, is installed and
# verifies. Its certificate has no key usage or extended key usage, which
# allows HTTPS. The CA-signed P-256 chain is installed over it below.
ca_md=sha384
sign_csr "$tmp/bmc.csr" "$tmp/int384" "$tmp/leaf384.pem" "$tmp/leaf-any.ext"
ca_md=sha256
cat "$tmp/leaf384.pem" "$tmp/int384.pem" >"$tmp/chain384.pem"
installed=1
replace_certificate "$tmp/chain384.pem"
[ "$code" = 204 ] || [ "$code" = 200 ] || fail "ReplaceCertificate with a P-384 chain: HTTP $code: $rf_body"
openssl s_client -connect "$IP:443" -CAfile "$tmp/root384.pem" -verify_return_error \
	-verify_hostname "$fqdn" </dev/null >"$tmp/s_client384.log" 2>&1 ||
	fail "TLS verification against the P-384 test root CA failed, see $tmp/s_client384.log"
[ "$(pin_of_server)" = "$orig_pin" ] || fail "the key changed"
ok "ReplaceCertificate installed a chain with a P-384 intermediate, and no key usage, which verifies"

# Install it
installed=1
replace_certificate "$tmp/chain.pem"
[ "$code" = 204 ] || [ "$code" = 200 ] || fail "ReplaceCertificate: HTTP $code: $rf_body"
ok "ReplaceCertificate installed the CA-signed certificate"

# The new certificate is served straight away, and verifies
[ "$(served_fingerprint)" = "$leaf_fp" ] || fail "served certificate isn't the one installed"
ok "the BMC serves the new certificate ($leaf_fp)"

[ "$(pin_of_server)" = "$orig_pin" ] || fail "the key changed"
ok "the key is unchanged"

openssl s_client -connect "$IP:443" -CAfile "$tmp/root.pem" -verify_return_error \
	-verify_hostname "$fqdn" </dev/null >"$tmp/s_client.log" 2>&1 ||
	fail "TLS verification against the test root CA failed, see $tmp/s_client.log"
grep -q '^Certificate chain' "$tmp/s_client.log" &&
	[ "$(grep -c '^ *[0-9] s:' "$tmp/s_client.log")" = 2 ] ||
	fail "the BMC doesn't send the leaf and intermediate"
ok "the certificate and chain verify against the test root CA for $fqdn"

openssl s_client -connect "$IP:443" -CAfile "$tmp/root.pem" -verify_return_error \
	-verify_ip "$IP" </dev/null >/dev/null 2>&1 ||
	fail "TLS verification of $IP against the test root CA failed"
ok "the certificate verifies for $IP"

curl -sf -m 30 --cacert "$tmp/root.pem" --resolve "$fqdn:443:$IP" \
	-u "$BMC_USER:$BMC_PASS" -o /dev/null "https://$fqdn/redfish/v1/" ||
	fail "curl with the test root CA (no -k) failed"
ok "curl verifies https://$fqdn/ with the test root CA"

rf GET "$CERT_PATH"
[ "$code" = 200 ] || fail "GET $CERT_PATH: HTTP $code"
[ "$(jq -r .Fingerprint <<<"$rf_body")" = "$leaf_fp" ] || fail "Redfish shows the wrong fingerprint"
[ "$(jq -r .Issuer.CommonName <<<"$rf_body")" = "WallaBMC Test Intermediate CA" ] ||
	fail "Redfish shows the wrong issuer: $(jq -c .Issuer <<<"$rf_body")"
[ "$(jq -r .Subject.CommonName <<<"$rf_body")" = "$fqdn" ] &&
	[ "$(jq -r .Subject.Organization <<<"$rf_body")" = "$org" ] ||
	fail "Redfish shows the wrong subject: $(jq -c .Subject <<<"$rf_body")"
ok "Redfish shows the new certificate"

# An installed certificate is kept when the hostname changes, even though it
# isn't valid for the new name (the BMC logs a warning, which we can't see)
test_host="${host:0:58}-test"
set_hostname "$test_host"
[ "$(served_fingerprint)" = "$leaf_fp" ] || fail "the installed certificate was replaced when the hostname changed"
ok "the installed certificate is kept when the hostname changes to $test_host"

if [ $keep = 1 ]; then
	set_hostname "$host"
	cat <<EOF
--keep: leaving the test certificate installed. To check it on the BMC's
serial console, run:

  uart:~\$ bmc cert

and check it shows:

  Source:              installed
  SHA-256 fingerprint: $leaf_fp
  Public key pin:      sha256//$orig_pin

To go back to a self-signed certificate, run on the serial console:

  uart:~\$ bmc cert self-signed

or:

  curl -k --pinnedpubkey 'sha256//$orig_pin' \\
      -u $BMC_USER:<password> -X DELETE $BASE$CERT_PATH
EOF
else
	# DELETE goes back to a self-signed certificate, with the same key
	rf DELETE "$CERT_PATH"
	[ "$code" = 204 ] || fail "DELETE $CERT_PATH: HTTP $code: $rf_body"
	installed=0
	rf GET "$CERT_PATH"
	[ "$code" = 200 ] || fail "GET $CERT_PATH after DELETE: HTTP $code"
	[ "$(jq -c .Subject <<<"$rf_body")" = "$(jq -c .Issuer <<<"$rf_body")" ] ||
		fail "the certificate isn't self-signed after DELETE"
	new_fp=$(jq -r .Fingerprint <<<"$rf_body")
	[ "$(served_fingerprint)" = "$new_fp" ] || fail "served certificate doesn't match Redfish after DELETE"
	[ "$new_fp" != "$leaf_fp" ] || fail "still serving the test certificate after DELETE"
	[ "$(pin_of_server)" = "$orig_pin" ] || fail "the key changed after DELETE"
	ok "DELETE put back a self-signed certificate with the same key ($new_fp)"

	# The self-signed certificate is for the current hostname
	served_names | grep -qx "DNS:$test_host" ||
		fail "the self-signed certificate isn't for the hostname $test_host"
	ok "the self-signed certificate is for the hostname $test_host"

	# Changing the hostname back reissues the self-signed certificate, with
	# the same key
	set_hostname "$host"
	reissued_fp=$(served_fingerprint)
	[ "$reissued_fp" != "$new_fp" ] || fail "the self-signed certificate wasn't reissued when the hostname changed"
	served_names >"$tmp/names.txt"
	grep -qx "DNS:$host" "$tmp/names.txt" || fail "the reissued certificate isn't for the hostname $host"
	! grep -qx "DNS:$test_host" "$tmp/names.txt" || fail "the reissued certificate is still for $test_host"
	[ "$(pin_of_server)" = "$orig_pin" ] || fail "the key changed on reissue"
	ok "the self-signed certificate is reissued for the hostname $host with the same key ($reissued_fp)"
fi

echo
echo "All $pass tests passed"
