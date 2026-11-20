#!/bin/bash
# SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Check that a live device does not offer TLS session resumption, using
# openssl s_client.
#
# WallaBMC does not support resumption: Zephyr's TLS sockets do not
# issue session tickets as a server, and the TLS 1.2 session cache is
# not enabled. Every connection is a full handshake. This test pins
# that down so a change in behaviour (for example after a Zephyr
# update) is noticed:
#
#  - TLS 1.3: the server must not send a NewSessionTicket. Without a
#    ticket there is nothing to resume and no way to send 0-RTT early
#    data.
#  - TLS 1.2: a session offered back to the server must not be resumed.
#
# Usage: test-tls-resumption.sh [IP]
#   IP     Target device address (default: 192.168.1.110)

set -u

usage() {
	cat <<'EOF'
Usage: test-tls-resumption.sh [IP]
  IP     Target device address (default: 192.168.1.110)
EOF
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

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

failed=0

pass() {
	echo "PASS: $*"
}

fail() {
	echo "FAIL: $*" >&2
	failed=$((failed + 1))
}

negotiated() {
	printf '%s\n' "$1" | grep -aE '^(New|Reused), ' | head -1
}

# Connect and save any session the server lets us keep. TLS 1.3 tickets
# arrive after the handshake, and some servers only send them with the
# first response, so make a request and hold the connection open for two
# seconds.
first_connection() {
	local ver="$1" sess="$2"
	(printf 'GET /redfish/v1/ HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n' "$IP"; sleep 2) |
		openssl s_client -connect "$IP:443" "-$ver" -sess_out "$sess" -ign_eof 2>&1
}

# TLS 1.3: s_client only writes the session file when a ticket arrives.
sess="$tmpdir/tls1_3.sess"
out="$(first_connection tls1_3 "$sess")"
if ! negotiated "$out" | grep -q '^New, TLSv1.3'; then
	fail "tls1_3 handshake"
elif [ -e "$sess" ]; then
	fail "tls1_3 server sent a session ticket"
	out="$(openssl s_client -connect "$IP:443" -tls1_3 -sess_in "$sess" </dev/null 2>&1)"
	if negotiated "$out" | grep -q '^Reused'; then
		fail "tls1_3 session was resumed"
	fi
else
	pass "tls1_3 server sends no session ticket"
fi

# TLS 1.2: the session is always saved, the server decides on resumption.
sess="$tmpdir/tls1_2.sess"
out="$(first_connection tls1_2 "$sess")"
if ! negotiated "$out" | grep -q '^New, TLSv1.2'; then
	fail "tls1_2 handshake"
else
	if grep -aq 'TLS session ticket:' <<<"$out"; then
		fail "tls1_2 server sent a session ticket"
	else
		pass "tls1_2 server sends no session ticket"
	fi
	out="$(openssl s_client -connect "$IP:443" -tls1_2 -sess_in "$sess" </dev/null 2>&1)"
	if negotiated "$out" | grep -q '^New, TLSv1.2'; then
		pass "tls1_2 session is not resumed"
	else
		fail "tls1_2 second connection: $(negotiated "$out")"
	fi
fi

echo
if [ "$failed" -ne 0 ]; then
	echo "$failed test(s) failed" >&2
	exit 1
fi
echo "All TLS resumption tests passed"
