#!/usr/bin/env bash
#-
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# examples/08_tnt/test_echo.sh -- end-to-end TCP echo round-trip test.
#
# Starts tnt_echo on a port, opens a TCP connection, sends several
# lines, and verifies each is echoed back byte-for-byte.  Uses only
# bash + /dev/tcp (no nc dependency).  Cleans up the server on exit.

set -u

PORT="${TNT_ECHO_PORT:-17777}"
SHARDS="${TNT_ECHO_SHARDS:-2}"
SERVER=./tnt_echo
FAIL=0

if [ ! -x "$SERVER" ]; then
	echo "FAIL: $SERVER not built" >&2
	exit 1
fi

# Start the server.
"$SERVER" "$PORT" "$SHARDS" &
SRV_PID=$!

cleanup() {
	kill "$SRV_PID" 2>/dev/null
	wait "$SRV_PID" 2>/dev/null
}
trap cleanup EXIT

# Wait for the listener to come up (retry connect for up to ~3s).
up=0
for i in $(seq 1 30); do
	if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
		up=1
		break
	fi
	sleep 0.1
done
if [ "$up" -ne 1 ]; then
	echo "FAIL: server did not come up on port $PORT" >&2
	exit 1
fi

# One connection, several round-trips.
roundtrip() {
	local msg="$1"
	local reply
	exec 9<>"/dev/tcp/127.0.0.1/$PORT" || {
		echo "FAIL: connect" >&2; return 1; }
	printf '%s' "$msg" >&9
	# Read back exactly ${#msg} bytes.
	IFS= read -r -N "${#msg}" -u 9 reply
	exec 9<&- 9>&-
	if [ "$reply" = "$msg" ]; then
		echo "ok: '$msg' -> '$reply'"
		return 0
	else
		echo "FAIL: '$msg' -> '$reply'" >&2
		return 1
	fi
}

for m in "hello" "tnt echo works" "0123456789" "the quick brown fox"; do
	roundtrip "$m" || FAIL=1
done

# A few concurrent connections to exercise multiple isolates / shards.
pids=()
for i in $(seq 1 8); do
	(
		exec 9<>"/dev/tcp/127.0.0.1/$PORT" || exit 1
		msg="conn-$i-payload"
		printf '%s' "$msg" >&9
		IFS= read -r -N "${#msg}" -u 9 reply
		exec 9<&- 9>&-
		[ "$reply" = "$msg" ] || exit 1
	) &
	pids+=($!)
done
conc_ok=1
for p in "${pids[@]}"; do
	wait "$p" || conc_ok=0
done
if [ "$conc_ok" -eq 1 ]; then
	echo "ok: 8 concurrent connections echoed correctly"
else
	echo "FAIL: concurrent connections" >&2
	FAIL=1
fi

if [ "$FAIL" -eq 0 ]; then
	echo ""
	echo "PASS: tnt TCP echo round-trips verified"
	exit 0
fi
echo ""
echo "FAIL: tnt echo test"
exit 1
