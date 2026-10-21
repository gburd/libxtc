#!/bin/bash
# test/sqlxtc/test_sqlxtc_logdrain.sh
#   The server's log reaches its sink WHILE the server runs.  Before,
#   nothing drained the xtc_log ring between startup and exit, so the
#   5-second metrics line and the loop stall watchdog's warning were
#   invisible until shutdown (and the ring dropped the oldest records
#   once 4096 had piled up).  Starts a server, waits 12 s, and requires
#   at least two metrics lines in its log before it is stopped; then
#   checks a SIGTERM still shuts it down promptly.
#
#   Portable; no setsid, no `timeout`.  Kills by explicit PID only.
set -u

PORT=${PORT:-15493}
DIR=$(cd "$(dirname "$0")" && pwd)
SVR_BIN=${SQLXTC_SERVER:-$DIR/../../examples/06_sqlxtc/sqlxtc-server}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/sqlxtc-logdrain.XXXXXX")
LOGFILE=$WORK/log

if [ ! -x "$SVR_BIN" ]; then
    echo "SKIP: $SVR_BIN not built"
    exit 0
fi

nohup "$SVR_BIN" -p "$PORT" -d "$WORK/t.db" < /dev/null > "$LOGFILE" 2>&1 &
SVR_PID=$!

cleanup() {
    kill -9 "$SVR_PID" 2>/dev/null || true
    rm -f "$WORK"/t.db* "$LOGFILE"
    rmdir "$WORK" 2>/dev/null || true
}
trap cleanup EXIT

sleep 12
if ! kill -0 "$SVR_PID" 2>/dev/null; then
    echo "FAIL: server exited early"
    cat "$LOGFILE"
    exit 1
fi
n=$(grep -c 'metrics:' "$LOGFILE")
if [ "$n" -lt 2 ]; then
    echo "FAIL: $n metrics lines in the log after 12 s of running (want >= 2)"
    exit 1
fi

kill -TERM "$SVR_PID"
t=0
while kill -0 "$SVR_PID" 2>/dev/null && [ $t -lt 100 ]; do
    sleep 0.1
    t=$((t + 1))
done
if kill -0 "$SVR_PID" 2>/dev/null; then
    echo "FAIL: server still running 10 s after SIGTERM"
    exit 1
fi
if ! grep -q 'storage quiesced' "$LOGFILE"; then
    echo "FAIL: no drain record in the log"
    exit 1
fi
echo "OK: $n metrics lines while running; SIGTERM drain in $((t * 100)) ms"
exit 0
