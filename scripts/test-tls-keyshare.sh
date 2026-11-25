#!/bin/bash
# SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Check which TLS 1.3 key exchange groups a live device accepts without
# a HelloRetryRequest, using openssl s_client.
#
# A TLS 1.3 client sends key shares for the groups it guesses the server
# supports. If the server supports none of them it answers with a
# HelloRetryRequest and the client has to send a second ClientHello,
# which costs a round trip on every connection. WallaBMC supports X25519
# and P-256, so a client offering either must connect with a single
# ClientHello.
#
# A client that only sends a P-521 share is used as a control: the
# device does not support P-521, so that handshake must need a retry.
# (Not P-384: enabling it, to install certificate chains with P-384 CAs,
# also makes it a supported key exchange group.)
# This shows the test can see a HelloRetryRequest.
#
# Usage: test-tls-keyshare.sh [IP]
#   IP     Target device address (default: 192.168.1.110)

set -u

usage() {
	cat <<'USAGE'
Usage: test-tls-keyshare.sh [IP]
  IP     Target device address (default: 192.168.1.110)
USAGE
}

IP=""
for arg in "$@"; do
	case "$arg" in
	-h|--help) usage; exit 0 ;;
	-*) echo "Unknown option: $arg" >&2; usage >&2; exit 1 ;;
	*) IP="$arg" ;;
	esac
done
IP="${IP:-192.168.1.110}"

if ! command -v openssl >/dev/null 2>&1; then
	echo "openssl not installed" >&2
	exit 1
fi

failed=0

pass() {
	echo "PASS: $*"
}

fail() {
	echo "FAIL: $*" >&2
	failed=$((failed + 1))
}

# check <description> <ClientHellos expected> <temp key pattern> [s_client args]
#
# s_client -msg prints one ">>> ... ClientHello" line for each ClientHello
# sent. Two means the server asked for a retry.
check() {
	local desc="$1" want_hellos="$2" want_key="$3"
	shift 3
	local out hellos key
	if ! out="$(openssl s_client -connect "$IP:443" -tls1_3 -msg "$@" </dev/null 2>&1)"; then
		fail "$desc: handshake failed"
		return
	fi
	hellos="$(grep -ac '^>>> .*ClientHello' <<<"$out")"
	key="$(grep -aE '^(Peer|Server) Temp Key:' <<<"$out" | head -1)"
	if [ "$hellos" != "$want_hellos" ]; then
		fail "$desc: $hellos ClientHello(s), expected $want_hellos"
	elif ! grep -q "$want_key" <<<"$key"; then
		fail "$desc: $key"
	else
		pass "$desc: $hellos ClientHello(s), $key"
	fi
}

check "X25519 key share" 1 'X25519' -groups X25519
check "P-256 key share" 1 'prime256v1' -groups P-256
check "openssl default key shares" 1 'Temp Key'
check "P-521 key share is retried (control)" 2 'X25519' -groups P-521:X25519

echo
if [ "$failed" -ne 0 ]; then
	echo "$failed test(s) failed" >&2
	exit 1
fi
echo "All TLS key share tests passed"
