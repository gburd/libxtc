#!/bin/bash
# test/sqlxtc/test_sqlxtc_budgets.sh -- enforce hard caps.
set -u

PORT=${PORT:-15435}
DIR=$(dirname "$0")
SVR_BIN=$DIR/../../examples/06_sqlxtc/sqlxtc-server
LOGFILE=/tmp/sqlxtc-budget.log
DBFILE=/tmp/sqlxtc-budget.db

die() { echo "FAIL: $*"; cleanup; exit 1; }

cleanup() {
    pkill -9 -f "sqlxtc-server.*-p $PORT" 2>/dev/null || true
    rm -f "$DBFILE" "$DBFILE-shm" "$DBFILE-wal"
}

start_server() {
    local args="$@"
    pkill -9 -f "sqlxtc-server.*-p $PORT" 2>/dev/null || true
    sleep 0.2
    rm -f "$DBFILE" "$DBFILE-shm" "$DBFILE-wal" "$LOGFILE"
    nohup setsid "$SVR_BIN" -p "$PORT" -d "$DBFILE" $args \
        < /dev/null > "$LOGFILE" 2>&1 &
    disown
    sleep 0.4
    pgrep -f "sqlxtc-server.*-p $PORT" >/dev/null || die "server did not start"
}

stop_server() {
    pkill -9 -f "sqlxtc-server.*-p $PORT" 2>/dev/null || true
    sleep 0.1
}

trap cleanup EXIT

# ---- max-clients ----
echo "=== test: --max-clients ==="
start_server -n 5
python3 - "$PORT" <<'PY'
import socket, sys, json
PORT = int(sys.argv[1])

socks = []
ok = 0
rejected = 0
for i in range(20):
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=2)
        f = s.makefile('rwb', buffering=0)
        line = f.readline()
        m = json.loads(line)
        if 'err' in m:
            rejected += 1; s.close()
        else:
            ok += 1
            socks.append(s)
    except Exception:
        rejected += 1

print(f"accepted={ok} rejected={rejected}")
for s in socks:
    s.close()
sys.exit(0 if ok <= 5 and rejected >= 15 else 1)
PY
[ $? -eq 0 ] || die "max-clients did not enforce"
stop_server
echo "OK: max-clients enforced"

# ---- max-iops ----
echo
echo "=== test: --max-iops ==="
start_server -i 5                        # 5 queries/sec/server
python3 - "$PORT" <<'PY'
import socket, sys, json, time
PORT = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
f = s.makefile('rwb', buffering=0)
f.readline()                               # banner
# Burst: write all queries first, then read all replies.
N = 100
batch = b'{"q":"SELECT 1"}\n' * N
f.write(batch)
ok = 0
limited = 0
for i in range(N):
    while True:
        line = f.readline()
        if not line: break
        m = json.loads(line)
        if 'cols' in m or 'row' in m: continue
        if 'done' in m: ok += 1; break
        if 'err' in m and m.get('err') == 'OVER_LIMIT':
            limited += 1; break
        if 'err' in m: limited += 1; break
print(f"ok={ok} limited={limited}")
sys.exit(0 if limited >= 50 else 1)
PY
[ $? -eq 0 ] || die "max-iops did not throttle"
stop_server
echo "OK: max-iops throttled"

echo
echo "=== test: --max-memory enforcement (informational) ==="
# We can't easily exhaust memory in a smoke test; just verify the
# server starts with a small cap and serves SELECT 1.
start_server -m 50000000
python3 - "$PORT" <<'PY'
import socket, sys, json
PORT = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
f = s.makefile('rwb', buffering=0)
f.readline()
f.write(b'{"q":"SELECT 1+1"}\n')
got_done = False
for _ in range(5):
    line = f.readline()
    if not line: break
    if b'"done"' in line: got_done = True; break
sys.exit(0 if got_done else 1)
PY
[ $? -eq 0 ] || die "small max-memory broke server"
stop_server
echo "OK: max-memory cap configured + server functional"

echo
echo "ALL budget tests passed"
exit 0
