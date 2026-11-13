#!/bin/bash
# SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Run Redfish API tests against a live device (or QEMU) using curl,
# redfishtool and hurl. Each tool is checked before use, so a subset of
# the tests still runs when some tools are not installed.
#
# By default only read-only tests run. Tests that change the board state
# (power on/off/cycle) are skipped unless --all is given.
#
# Usage: test-redfish.sh [--all] [IP]
#   --all  Also run tests that change board state (power on/off/cycle)
#   IP     Target device address (default: 192.168.1.110)

set -u

USER=admin
PASS=admin

usage() {
	cat <<'EOF'
Usage: test-redfish.sh [--all] [IP]
  --all  Also run tests that change board state (power on/off/cycle)
  IP     Target device address (default: 192.168.1.110)
EOF
}

run_all=0
have_https=1
IP=""
for arg in "$@"; do
	case "$arg" in
	--all) run_all=1 ;;
	-h|--help) usage; exit 0 ;;
	-*) echo "Unknown option: $arg" >&2; usage >&2; exit 1 ;;
	*) IP="$arg" ;;
	esac
done
IP="${IP:-192.168.1.110}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HURL_FILE="$SCRIPT_DIR/test.hurl"
HURL_REDIRECT_FILE="$SCRIPT_DIR/test-redirect.hurl"

# Prefer tools (redfishtool) from the scripts venv when it exists.
# Install redfishtool there with: scripts/.venv/bin/pip install redfishtool
VENV="$SCRIPT_DIR/.venv"
if [ -d "$VENV/bin" ]; then
	PATH="$VENV/bin:$PATH"
fi

ran=0
skipped=0

# Abort the whole run on the first test failure.
fail() {
	echo "FAIL: $*" >&2
	exit 1
}

have() {
	if command -v "$1" >/dev/null 2>&1; then
		return 0
	fi
	echo ">>> $1 not installed, skipping ${2:-$1} tests"
	skipped=$((skipped + 1))
	return 1
}

run_curl_tests() {
	have curl || return
	local url=$1

	# jq is only used to pretty-print; fall back to raw output without it.
	local pp=cat
	if command -v jq >/dev/null 2>&1; then
		pp=jq
	else
		echo ">>> jq not installed, curl output will not be pretty-printed"
	fi

	echo ">>> curl tests $url"
	for path in \
		/redfish \
		/redfish/ \
		/redfish/v1 \
		/redfish/v1/ \
		/redfish/v1/Systems; do
		echo "--- GET $url$path"
		curl -Lsk "$url$path" | "$pp"
	done
	ran=$((ran + 1))
}

# curl-based auth checks. These can't live in test.hurl because that file
# is run with a global "-u admin:admin", which forces credentials onto
# every request and defeats the point of the negative tests.
run_auth_tests() {
	have curl auth || return

	echo ">>> auth rejection tests"

	# An auth-required endpoint with no credentials must return 401 and
	# advertise Basic auth via the WWW-Authenticate header.
	local out code location
	location="http://$IP/redfish/v1/Managers"
	out="$(curl -sk -D - -o /dev/null "$location")"
	# Strip the trailing CR that terminates HTTP header lines.
	code="$(printf '%s' "$out" | awk 'NR==1{print $2}' | tr -d '\r')"
	if [ "$code" = 308 ]; then
		location=$(printf '%s' "$out" | awk 'tolower($1) == "location:" {print $2}' | tr -d '\r')
		if [ -z "$location" ]; then
			fail "redirect but no Location?"
		fi
		out="$(curl -sk -D - -o /dev/null "$location")"
		# Strip the trailing CR that terminates HTTP header lines.
		code="$(printf '%s' "$out" | awk 'NR==1{print $2}' | tr -d '\r')"
	fi

	if [ "$code" = 401 ] && printf '%s' "$out" | grep -qi '^www-authenticate:[[:space:]]*Basic'; then
		echo "PASS: no credentials -> 401 + WWW-Authenticate"
	else
		fail "no credentials -> ${code:-?} (WWW-Authenticate missing?)"
	fi

	# Wrong credentials must also be rejected with 401.
	code="$(curl -sk -o /dev/null -w '%{http_code}' -u "$USER:wrongpassword" "$location")"
	if [ "$code" = 401 ]; then
		echo "PASS: wrong password -> 401"
	else
		fail "wrong password -> $code"
	fi

	ran=$((ran + 1))
}

run_redfishtool_tests() {
	have redfishtool || return

	echo ">>> redfishtool tests"
	# A TLS handshake with the BMC can take several seconds, longer than
	# redfishtool's default 3 second connect timeout (-W 1:3), which it
	# also applies to the handshake. Allow 10 seconds.
	local rt=(redfishtool -r "$IP" -u "$USER" -p "$PASS" -W 1:10 -vvvv)
	local cmd
	# $cmd is intentionally unquoted so multi-word commands split into args.
	for cmd in \
		"root" \
		"versions" \
		"Systems" \
		"Systems -I system" \
		"Managers" \
		"Chassis" \
		"AccountService"; do
		echo "--- redfishtool $cmd"
		"${rt[@]}" $cmd || fail "redfishtool $cmd (exit $?)"
	done

	# These change the host power state, so only run them on request.
	if [ "$run_all" -eq 1 ]; then
		for cmd in \
			"Systems reset On" \
			"Systems reset ForceOff" \
			"Systems reset On" \
			"Systems reset ForceRestart" \
			"Systems reset PowerCycle"; do
			echo "--- redfishtool $cmd"
			"${rt[@]}" $cmd || fail "redfishtool $cmd (exit $?)"
		done
	else
		echo ">>> skipping power state changes (pass --all to enable)"
	fi
	ran=$((ran + 1))
}

run_hurl_tests() {
	have hurl || return
	local base=$1

	echo ">>> hurl tests $base"
	# --location-trusted follows the redirect to HTTPS (CONFIG_APP_HTTP=n)
	# and resends the credentials, which curl otherwise drops when a
	# redirect changes the scheme.
	hurl --insecure --location-trusted -u "$USER:$PASS" --pretty -v \
		--variable base="$base" --test "$HURL_FILE" ||
		fail "hurl tests $base (exit $?)"
	ran=$((ran + 1))
}

# Checks the 308 itself, so run without following redirects.
run_hurl_redirect_tests() {
	have hurl "hurl redirect" || return

	echo ">>> hurl redirect tests"
	hurl --variable host="$IP" --variable user="$USER:$PASS" --pretty -v \
		--test "$HURL_REDIRECT_FILE" ||
		fail "hurl redirect tests (exit $?)"
	ran=$((ran + 1))
}

code="$(curl -sk -o /dev/null -w '%{http_code}' "https://$IP/redfish/v1")"
if [ "$code" != 200 ]; then
	have_https=0
fi

# Plain HTTP redirects to HTTPS when built with CONFIG_APP_HTTP=n.
http_redirects=0
code="$(curl -s -o /dev/null -w '%{http_code}' "http://$IP/redfish/v1")"
if [ "$code" = 308 ]; then
	http_redirects=1
	[ "$have_https" -eq 1 ] || fail "HTTP redirects to HTTPS but HTTPS isn't working"
fi

run_curl_tests "http://$IP"
if [ "$have_https" -eq 1 ]; then
	run_curl_tests "https://$IP"
fi

run_auth_tests
run_redfishtool_tests

run_hurl_tests "http://$IP"
if [ "$have_https" -eq 1 ]; then
	run_hurl_tests "https://$IP"
fi
if [ "$http_redirects" -eq 1 ]; then
	run_hurl_redirect_tests
fi

echo ">>> done: $ran test group(s) ran, $skipped skipped"
if [ "$ran" -eq 0 ]; then
	echo ">>> no test tools installed" >&2
	exit 1
fi
