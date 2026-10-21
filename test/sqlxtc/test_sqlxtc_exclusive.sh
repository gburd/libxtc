#!/bin/bash
# test/sqlxtc/test_sqlxtc_exclusive.sh
#   One server per store, one server per port.
#
#   (1) A second sqlxtc-server started on the SAME --db (another port)
#       used to start: it found the base not marked clean, truncated the
#       page file, rebuilt from the log and renamed a compacted log over
#       the first server's, so a commit the FIRST server acknowledged
#       afterwards went to an unlinked file and was gone after a restart.
#       The second server must refuse to start, and the first server's
#       acknowledged commit must survive its crash.
#   (2) A second server on the SAME port (another --db) used to bind too
#       (SO_REUSEPORT), and the kernel split new connections between the
#       two stores.  It must fail to start.
#
#   Portable; no setsid, no `timeout`.  Kills by explicit PID only.
set -u

PORT=${PORT:-15494}
DIR=$(cd "$(dirname "$0")" && pwd)
SVR_BIN=${SQLXTC_SERVER:-$DIR/../../examples/06_sqlxtc/sqlxtc-server}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/sqlxtc-excl.XXXXXX")
PIDS=""

if [ ! -x "$SVR_BIN" ]; then
    echo "SKIP: $SVR_BIN not built"
    exit 0
fi

cleanup() {
    for p in $PIDS; do kill -9 "$p" 2>/dev/null || true; done
    rm -f "$WORK"/*
    rmdir "$WORK" 2>/dev/null || true
}
trap cleanup EXIT

fail() { echo "FAIL: $*"; exit 1; }

# q PORT 'SQL;;SQL...' -> prints one result token per statement
q() {
    python3 - "$1" "$2" <<'PY'
import json, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10)
f = s.makefile("rwb", buffering=0)
f.readline()
out = []
for sql in sys.argv[2].split(";;"):
    f.write((json.dumps({"q": sql}) + "\n").encode())
    while True:
        m = json.loads(f.readline())
        if "row" in m:
            out.append(str(m["row"][0]))
        if "done" in m:
            out.append("ok")
            break
        if "err" in m:
            out.append("err")
            break
print(" ".join(out))
PY
}

wait_up() {   # wait_up PORT PID
    local i
    for i in $(seq 1 50); do
        kill -0 "$2" 2>/dev/null || return 1
        python3 -c "import socket,sys; socket.create_connection(('127.0.0.1',$1),timeout=1)" \
            2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

# (1) same store
nohup "$SVR_BIN" -p "$PORT" -d "$WORK/x.db" < /dev/null > "$WORK/a.log" 2>&1 &
A=$!; PIDS="$PIDS $A"
wait_up "$PORT" "$A" || fail "server A did not start"
[ "$(q "$PORT" "CREATE TABLE t(k INTEGER PRIMARY KEY, v TEXT);;INSERT INTO t VALUES(1, 'one')")" = "ok ok" ] \
    || fail "setup"
nohup "$SVR_BIN" -p $((PORT + 1)) -d "$WORK/x.db" < /dev/null > "$WORK/b.log" 2>&1 &
B=$!; PIDS="$PIDS $B"
sleep 2
if kill -0 "$B" 2>/dev/null; then
    b_up=1
else
    b_up=0
fi
[ "$(q "$PORT" "INSERT INTO t VALUES(2, 'two')")" = "ok" ] || fail "A did not acknowledge"
kill -9 "$A"; wait "$A" 2>/dev/null
kill -9 "$B" 2>/dev/null; wait "$B" 2>/dev/null
nohup "$SVR_BIN" -p "$PORT" -d "$WORK/x.db" < /dev/null > "$WORK/c.log" 2>&1 &
C=$!; PIDS="$PIDS $C"
wait_up "$PORT" "$C" || fail "restart did not start: $(tail -2 "$WORK/c.log")"
got=$(q "$PORT" "SELECT count(*) FROM t WHERE k = 2")
[ "$b_up" = 0 ] || fail "a second server on the same store started (acked row present after restart: $got)"
[ "$got" = "1 ok" ] || fail "server A's acknowledged commit lost after restart ($got)"
echo "OK: a second server on the same store refused; the acknowledged commit survived"

# (2) same port, another store
nohup "$SVR_BIN" -p "$PORT" -d "$WORK/y.db" < /dev/null > "$WORK/d.log" 2>&1 &
D=$!; PIDS="$PIDS $D"
for _ in $(seq 1 60); do     # it retries the bind for up to 2 s
    kill -0 "$D" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$D" 2>/dev/null; then
    fail "a second server bound the same port (connections would be split)"
fi
echo "OK: a second server on the same port refused"
kill -TERM "$C"
exit 0
