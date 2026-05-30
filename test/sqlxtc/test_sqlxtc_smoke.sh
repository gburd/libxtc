#!/bin/bash
# examples/06_sqlxtc/test/test_sqlxtc_smoke.sh -- end-to-end smoke test.
# SAFETY: uses the documented `nohup setsid ... & disown` pattern.
# NEVER use `timeout N ./binary` here; that has caused multi-hour hangs.
set -u

PORT=${PORT:-15433}
DIR=$(dirname "$0")
SVR_BIN=${SQLXTC_SERVER:-$DIR/../../examples/06_sqlxtc/sqlxtc-server}
PIDFILE=/tmp/sqlxtc-smoke.pid
LOGFILE=/tmp/sqlxtc-smoke.log

if [ ! -x "$SVR_BIN" ]; then
    echo "FAIL: $SVR_BIN not built"
    exit 1
fi

# Kill stragglers from a previous run.
pkill -9 -f "sqlxtc-server.*-p $PORT" 2>/dev/null || true
rm -f "$PIDFILE" "$LOGFILE"

# Background the server.
nohup setsid "$SVR_BIN" -p "$PORT" -d :memory: \
    < /dev/null > "$LOGFILE" 2>&1 &
disown
# Capture child via setsid -> use pgrep for the actual server.
sleep 0.4
SVR_PID=$(pgrep -f "sqlxtc-server.*-p $PORT" | head -1)
if [ -z "$SVR_PID" ]; then
    echo "FAIL: server did not start"
    cat "$LOGFILE"
    exit 1
fi
echo "$SVR_PID" > "$PIDFILE"

cleanup() {
    kill -9 "$SVR_PID" 2>/dev/null || true
    rm -f "$PIDFILE"
}
trap cleanup EXIT

# Run the python checks.
python3 "$DIR/test_sqlxtc_smoke.py" "$PORT"
ST=$?
exit $ST
