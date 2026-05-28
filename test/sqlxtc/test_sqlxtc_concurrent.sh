#!/bin/bash
# test/sqlxtc/test_sqlxtc_concurrent.sh -- Phase 3 concurrent stress.
set -u

PORT=${PORT:-15434}
DIR=$(dirname "$0")
SVR_BIN=$DIR/../../examples/06_sqlxtc/sqlxtc-server
PIDFILE=/tmp/sqlxtc-conc.pid
LOGFILE=/tmp/sqlxtc-conc.log
DBFILE=/tmp/sqlxtc-conc.db
CLIENTS=${CLIENTS:-100}
QUERIES=${QUERIES:-100}

if [ ! -x "$SVR_BIN" ]; then
    echo "FAIL: $SVR_BIN not built"; exit 1
fi

pkill -9 -f "sqlxtc-server.*-p $PORT" 2>/dev/null || true
rm -f "$PIDFILE" "$LOGFILE" "$DBFILE" "$DBFILE-shm" "$DBFILE-wal"

nohup setsid "$SVR_BIN" -p "$PORT" -d "$DBFILE" -n 200 \
    < /dev/null > "$LOGFILE" 2>&1 &
disown

sleep 0.5
SVR_PID=$(pgrep -f "sqlxtc-server.*-p $PORT" | head -1)
if [ -z "$SVR_PID" ]; then
    echo "FAIL: server did not start"; cat "$LOGFILE"; exit 1
fi
echo "$SVR_PID" > "$PIDFILE"

cleanup() {
    kill -9 "$SVR_PID" 2>/dev/null || true
    rm -f "$DBFILE" "$DBFILE-shm" "$DBFILE-wal" "$PIDFILE"
}
trap cleanup EXIT

python3 "$DIR/test_sqlxtc_concurrent.py" "$PORT" "$CLIENTS" "$QUERIES"
ST=$?

# Show server log on failure.
if [ $ST -ne 0 ]; then
    echo "--- server log ---"; tail -50 "$LOGFILE"
fi
exit $ST
