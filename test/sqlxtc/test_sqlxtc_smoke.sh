#!/bin/bash
# examples/06_sqlxtc/test/test_sqlxtc_smoke.sh -- end-to-end smoke test.
# SAFETY: backgrounds the server with `nohup ... &` and captures its PID
# directly from $!.  Does NOT use `setsid`: that is a Linux coreutils
# binary absent on macOS/BSD, where it made this harness fail with
# "setsid: No such file or directory" (the server never started).  nohup
# alone already detaches from the controlling terminal; the EXIT trap
# below reaps the exact PID, so no session/process-group juggling or
# pgrep guessing is needed.  Matches the portable pattern in
# test_sqlxtc_mt.sh.
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

# Background the server and capture its PID directly from $! (portable;
# no setsid, no pgrep).
nohup "$SVR_BIN" -p "$PORT" -d :memory: \
    < /dev/null > "$LOGFILE" 2>&1 &
SVR_PID=$!
echo "$SVR_PID" > "$PIDFILE"

cleanup() {
    kill -9 "$SVR_PID" 2>/dev/null || true
    rm -f "$PIDFILE"
}
trap cleanup EXIT

# Give it a moment to bind, then confirm it is still alive AND actually
# accepting connections.  A fixed sleep races the listen socket becoming
# ready (the xstore engine open takes a beat), so poll the port instead:
# portable, and no longer flaky on a slow or loaded host.
ready=0
for _ in $(seq 1 50); do          # up to ~5s
    if ! kill -0 "$SVR_PID" 2>/dev/null; then
        echo "FAIL: server did not start"
        cat "$LOGFILE"
        exit 1
    fi
    if python3 - "$PORT" <<'PY' 2>/dev/null
import socket, sys
try:
    socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=1).close()
except OSError:
    sys.exit(1)
PY
    then
        ready=1
        break
    fi
    sleep 0.1
done
if [ "$ready" -ne 1 ]; then
    echo "FAIL: server did not become ready"
    cat "$LOGFILE"
    exit 1
fi

# Run the python checks.
python3 "$DIR/test_sqlxtc_smoke.py" "$PORT"
ST=$?
exit $ST
