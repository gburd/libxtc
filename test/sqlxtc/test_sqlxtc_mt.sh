#!/bin/bash
# test/sqlxtc/test_sqlxtc_mt.sh
#   Multi-threaded (multi-loop) regression guard: run sqlxtc-server with
#   one worker loop per host CPU under concurrent load and assert it does
#   not crash. Guards the cross-loop xtc_amutex hand-off bug.
#
#   Portable (Linux + macOS), bounded, and never uses setsid or
#   `timeout` (both have bitten this tree). A hard watchdog guarantees
#   termination; the client driver runs a fixed query count and exits.
set -u

PORT=${PORT:-15490}
DIR=$(cd "$(dirname "$0")" && pwd)
SVR_BIN=${SQLXTC_SERVER:-$DIR/../../examples/06_sqlxtc/sqlxtc-server}
DBFILE=${TMPDIR:-/tmp}/sqlxtc-mt.$$.db
LOGFILE=${TMPDIR:-/tmp}/sqlxtc-mt.$$.log
CLIENTS=${CLIENTS:-64}
QUERIES=${QUERIES:-150}

if [ ! -x "$SVR_BIN" ]; then
    echo "SKIP: $SVR_BIN not built"
    exit 0
fi

# One loop per CPU.
if command -v nproc >/dev/null 2>&1; then
    CORES=$(nproc)
elif command -v sysctl >/dev/null 2>&1; then
    CORES=$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)
else
    CORES=2
fi
[ "$CORES" -ge 1 ] 2>/dev/null || CORES=2

pkill -9 -f "sqlxtc-server.*-p $PORT" 2>/dev/null || true
rm -f "$DBFILE" "$DBFILE"-shm "$DBFILE"-wal "$LOGFILE"

nohup "$SVR_BIN" -p "$PORT" -d "$DBFILE" -t "$CORES" -n 256 \
    -m $((512*1024*1024)) < /dev/null > "$LOGFILE" 2>&1 &
disown
SVR_PID=$!

# Hard watchdog: never let this test outlive ~60s.
( sleep 60; kill -9 "$SVR_PID" 2>/dev/null ) &
WD_PID=$!

cleanup() {
    kill -9 "$SVR_PID" "$WD_PID" 2>/dev/null || true
    rm -f "$DBFILE" "$DBFILE"-shm "$DBFILE"-wal "$LOGFILE"
}
trap cleanup EXIT

sleep 1
if ! kill -0 "$SVR_PID" 2>/dev/null; then
    echo "FAIL: server did not start (cores=$CORES)"
    cat "$LOGFILE"
    exit 1
fi
echo "server up: cores=$CORES pid=$SVR_PID"

python3 "$DIR/test_sqlxtc_concurrent.py" "$PORT" "$CLIENTS" "$QUERIES"
CLI_ST=$?

# The crash manifests as the server dying mid-load; assert it survived.
if ! kill -0 "$SVR_PID" 2>/dev/null; then
    echo "FAIL: server CRASHED under multi-loop load (cores=$CORES)"
    tail -30 "$LOGFILE"
    exit 1
fi

if [ "$CLI_ST" -ne 0 ]; then
    echo "FAIL: concurrent client reported errors (status $CLI_ST)"
    tail -30 "$LOGFILE"
    exit 1
fi

echo "OK: sqlxtc survived $CLIENTS-client load at -t $CORES"
exit 0
