#!/bin/bash
# examples/07_kaka/test_kaka_soak.sh
#   Concurrent network + storage soak: a real kaka-broker on a durable
#   data directory (fdatasync before every produce ack, small segments so
#   segments roll and cold FETCHes read them back through xtc_aio), with
#   PRODUCERS producers and CONSUMERS consumers on TOPICS topics running
#   AT THE SAME TIME over TCP.  kaka_soak checks every offset, sequence
#   and value byte, and that each consumer ends at exactly what was acked.
#
#   Env: SECS (10) TOPICS (4) PRODUCERS (8) CONSUMERS (8) VALUE (512)
#        BATCH (32) SEGMENT (262144) PORT (20000 + pid % 20000) TMPDIR
#        SOAK_WRAP: a command prefix for the broker, e.g. the io_uring
#        gate runs it as SOAK_WRAP="uringscope -f --json=r.json --".
#   Portable (Linux + macOS); never uses setsid or `timeout`.  A watchdog
#   kills the broker if the run outlives SECS + 60 s.
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
BROKER=${KAKA_BROKER:-$DIR/kaka-broker}
SOAK=${KAKA_SOAK:-$DIR/kaka_soak}
SECS=${SECS:-10}
TOPICS=${TOPICS:-4}
PRODUCERS=${PRODUCERS:-8}
CONSUMERS=${CONSUMERS:-8}
VALUE=${VALUE:-512}
BATCH=${BATCH:-32}
SEGMENT=${SEGMENT:-262144}
PORT=${PORT:-$((20000 + $$ % 20000))}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/kaka-soak.XXXXXX")
DATA=$WORK/data
LOG=$WORK/broker.log

for b in "$BROKER" "$SOAK"; do
	if [ ! -x "$b" ]; then
		echo "SKIP: $b not built (make -C examples/07_kaka)"
		rmdir "$WORK" 2>/dev/null
		exit 0
	fi
done
mkdir -p "$DATA"
# Refuse a port something already listens on: a stale broker there would
# answer this run's clients with another run's topics.
if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
	echo "FAIL: port $PORT is already in use (a stale broker?); set PORT"
	rmdir "$DATA" "$WORK" 2>/dev/null
	exit 1
fi

# shellcheck disable=SC2086  # SOAK_WRAP is a word list
${SOAK_WRAP:-} "$BROKER" -h 127.0.0.1 -p "$PORT" -d "$DATA" \
    --segment-bytes="$SEGMENT" < /dev/null > "$LOG" 2>&1 &
WRAP_PID=$!
( sleep $((SECS + 60)); kill -9 "$WRAP_PID" 2>/dev/null ) &
WD_PID=$!

# The server process: WRAP_PID itself, or under a SOAK_WRAP wrapper a
# descendant of it (pgrep -P works on Linux, macOS and the BSDs).
server_pid() {
	p=$WRAP_PID
	while [ -n "$p" ] && [ "$(ps -o comm= -p "$p" 2>/dev/null)" != kaka-broker ]; do
		p=$(pgrep -P "$p" 2>/dev/null | head -1)
	done
	echo "$p"
}

cleanup() {
	# every exit path, a failed one too, stops the server it started
	sp=$(server_pid)
	[ -n "$sp" ] && kill -TERM "$sp" 2>/dev/null
	wait "$WRAP_PID" 2>/dev/null
	kill "$WD_PID" 2>/dev/null
	wait "$WD_PID" 2>/dev/null
	find "$WORK" -mindepth 1 -delete 2>/dev/null
	rmdir "$WORK" 2>/dev/null
}
trap cleanup EXIT

# Wait for the listener (up to 10 s).
i=0
until (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; do
	i=$((i + 1))
	if [ "$i" -ge 100 ] || ! kill -0 "$WRAP_PID" 2>/dev/null; then
		echo "FAIL: kaka-broker did not start"
		cat "$LOG"
		exit 1
	fi
	sleep 0.1
done

"$SOAK" 127.0.0.1 "$PORT" "$TOPICS" "$PRODUCERS" "$CONSUMERS" "$SECS" \
    "$VALUE" "$BATCH"
ST=$?
SEGS=$(find "$DATA" -name '*.log' 2>/dev/null | wc -l | tr -d ' ')
BYTES=$(du -sk "$DATA" 2>/dev/null | cut -f1)

# Stop the broker cleanly (SIGTERM drains) and let any wrapper (sudo,
# uringscope) finish its report.
BPID=$(server_pid)
[ -n "$BPID" ] && kill -TERM "$BPID" 2>/dev/null
wait "$WRAP_PID" 2>/dev/null
WST=$?

if [ "$ST" -ne 0 ]; then
	echo "FAIL: kaka_soak reported errors (status $ST)"
	tail -20 "$LOG"
	exit 1
fi
if [ "$SEGS" -lt 2 ]; then
	echo "FAIL: only $SEGS segment file(s): segments never rolled," \
	    "so no cold read was exercised"
	exit 1
fi
if [ -n "${SOAK_WRAP:-}" ] && [ "$WST" -ne 0 ] && [ "$WST" -ne 143 ]; then
	echo "FAIL: the broker wrapper exited $WST"
	exit 1
fi
echo "OK: kaka soak, $PRODUCERS producers + $CONSUMERS consumers on" \
    "$TOPICS topics for ${SECS}s; $SEGS segments, ${BYTES} KiB on disk"
