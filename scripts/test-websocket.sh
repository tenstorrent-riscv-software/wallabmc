#!/bin/bash
# SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Check the websocket opening handshake on a live device with curl.
#
# The server must answer an upgrade request with 101 and a
# Sec-WebSocket-Accept header. That header is the base64 encoded SHA-1
# of the client's key and a fixed GUID (RFC 6455), so a correct value
# shows the firmware has working SHA-1 and base64 support. The key used
# here is the example from RFC 6455 section 1.3, and the expected answer
# is computed from it with openssl.
#
# Only the handshake is tested. The connection is closed before the
# "Auth:" message the consoles expect, so no console session is started.
#
# Usage: test-websocket.sh [--http-only|--https-only] [IP]
#   --http-only   Device was built with CONFIG_APP_HTTPS=n
#   --https-only  Only test wss:// (HTTP redirects to HTTPS)
#   IP            Target device address (default: 192.168.1.110)

set -u

for tool in curl openssl; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "$tool not installed" >&2
		exit 1
	fi
done

# Derive the key and expected answer rather than spelling out the base64
# strings, which secret scanners mistake for credentials.
GUID='258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
KEY="$(printf 'the sample nonce' | openssl base64)"
ACCEPT="$(printf '%s%s' "$KEY" "$GUID" | openssl dgst -sha1 -binary | openssl base64)"

usage() {
	cat <<'USAGE'
Usage: test-websocket.sh [--http-only|--https-only] [IP]
  --http-only   Device was built with CONFIG_APP_HTTPS=n
  --https-only  Only test wss:// (HTTP redirects to HTTPS)
  IP            Target device address (default: 192.168.1.110)
USAGE
}

schemes="http https"
IP=""
for arg in "$@"; do
	case "$arg" in
	--http-only) schemes="http" ;;
	--https-only) schemes="https" ;;
	-h|--help) usage; exit 0 ;;
	-*) echo "Unknown option: $arg" >&2; usage >&2; exit 1 ;;
	*) IP="$arg" ;;
	esac
done
IP="${IP:-192.168.1.110}"

failed=0

pass() {
	echo "PASS: $*"
}

fail() {
	echo "FAIL: $*" >&2
	failed=$((failed + 1))
}

# The server keeps the websocket open after the handshake, so curl is
# stopped by --max-time once the response headers have arrived.
upgrade() {
	curl -sk --http1.1 --max-time 5 -o /dev/null -D - \
		-H 'Connection: Upgrade' \
		-H 'Upgrade: websocket' \
		-H 'Sec-WebSocket-Version: 13' \
		-H "Sec-WebSocket-Key: $KEY" \
		"$1" | tr -d '\r'
}

for scheme in $schemes; do
	for path in /console/bmc /console/host; do
		url="$scheme://$IP$path"
		headers="$(upgrade "$url")"
		code="$(printf '%s\n' "$headers" | awk 'NR==1{print $2}')"
		accept="$(printf '%s\n' "$headers" |
			awk 'tolower($1)=="sec-websocket-accept:"{print $2}')"
		if [ "$code" != 101 ]; then
			fail "$url -> ${code:-no response}, expected 101"
		elif [ "$accept" != "$ACCEPT" ]; then
			fail "$url Sec-WebSocket-Accept is '${accept}'"
		else
			pass "$url upgrades with a correct Sec-WebSocket-Accept"
		fi
	done
done

echo
if [ "$failed" -ne 0 ]; then
	echo "$failed test(s) failed" >&2
	exit 1
fi
echo "All websocket tests passed"
