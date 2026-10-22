#!/bin/bash
# test/sqlxtc/test_sqlxtc_maxmem.sh
#   --max-memory bounds the memory the connections' buffers take.  It
#   used to bound nothing: the cap was checked against xtc_res's
#   MEM_BYTES counter, but nothing in sqlxtc ever charged that counter,
#   so it read 0 forever (the metrics line's mem=0) and a SELECT emitted
#   its whole result into the connection's write buffer no matter how
#   big.  Now every byte a connection buffer grows past its base is
#   charged; a result that would pass the cap is dropped and answered
#   with {"err":"OOM"} (then the connection is closed, per PROTOCOL.md).
#
#   Loads 2000 rows of 1000 bytes (a ~2 MB result), then:
#     1. -m 1048576: SELECT * gets {"err":"OOM"} and no rows, and the
#        connection is closed after it;
#     2. the budget comes back: a new connection's query succeeds and the
#        next metrics line reports mem=0;
#     3. control, no -m: the same SELECT returns all 2000 rows.
#
#   Portable; no setsid, no `timeout`.  Kills by explicit PID only.
set -u

PORT=${PORT:-15494}
DIR=$(cd "$(dirname "$0")" && pwd)
SVR_BIN=${SQLXTC_SERVER:-$DIR/../../examples/06_sqlxtc/sqlxtc-server}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/sqlxtc-maxmem.XXXXXX")
SVR_PID=

if [ ! -x "$SVR_BIN" ]; then
    echo "SKIP: $SVR_BIN not built"
    exit 0
fi

cleanup() {
    [ -n "$SVR_PID" ] && kill -9 "$SVR_PID" 2>/dev/null
    rm -f "$WORK"/t.db* "$WORK"/log
    rmdir "$WORK" 2>/dev/null || true
}
trap cleanup EXIT

start() {
    nohup "$SVR_BIN" -p "$PORT" -d "$WORK/t.db" "$@" < /dev/null \
        > "$WORK/log" 2>&1 &
    SVR_PID=$!
}

stop() {
    local t=0
    kill -TERM "$SVR_PID" 2>/dev/null
    while kill -0 "$SVR_PID" 2>/dev/null && [ $t -lt 100 ]; do
        sleep 0.1; t=$((t + 1))
    done
    kill -9 "$SVR_PID" 2>/dev/null
    wait "$SVR_PID" 2>/dev/null
    SVR_PID=
}

# client.py PORT MODE: load | big (SELECT *; prints "rows=N" or "err=E",
# then "eof" if the server closed the connection) | count
cat > "$WORK/client.py" <<'PY'
import json, socket, sys, time
port, mode = int(sys.argv[1]), sys.argv[2]
for _ in range(100):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=30); break
    except OSError:
        time.sleep(0.1)
f = s.makefile("rwb", buffering=0)
f.readline()
def q(sql, params=None):
    m = {"q": sql}
    if params: m["params"] = params
    f.write((json.dumps(m) + "\n").encode())
    rows = 0
    while True:
        line = f.readline()
        if not line: return ("eof", rows)
        r = json.loads(line)
        if "row" in r: rows += 1
        if "done" in r: return ("done", rows)
        if "err" in r: return ("err:" + r["err"], rows)
if mode == "load":
    q("CREATE TABLE t(k INTEGER PRIMARY KEY, pad TEXT)")
    q("BEGIN")
    for i in range(2000):
        q("INSERT INTO t VALUES(?, ?)", [i, "x" * 1000])
    print(q("COMMIT")[0])
elif mode == "big":
    kind, rows = q("SELECT * FROM t")
    print(f"{kind} rows={rows}")
    # closed = EOF, or a reset (the close found our ping unread)
    try:
        f.write(b'{"ping":1}\n')
        print("eof" if not f.readline() else "open")
    except OSError:
        print("eof")
else:
    f.write(b'{"q":"SELECT count(*) FROM t"}\n')
    print(f.readline().decode().strip(), f.readline().decode().strip())
PY

fail() { echo "FAIL: $*"; cat "$WORK/log"; exit 1; }

start
r=$(python3 "$WORK/client.py" "$PORT" load) || fail "load: $r"
[ "$r" = done ] || fail "load: $r"
c=$(python3 "$WORK/client.py" "$PORT" big)
echo "  no cap:   $c" | tr '\n' ' '; echo
case "$c" in "done rows=2000"*) ;; *) fail "uncapped SELECT: $c";; esac
stop

start -m 1048576
c=$(python3 "$WORK/client.py" "$PORT" big)
echo "  -m 1 MiB: $c" | tr '\n' ' '; echo
case "$c" in
"err:OOM rows=0"*eof) ;;
*) fail "a ~2 MB result under a 1 MiB --max-memory: $c (want err:OOM rows=0, then the connection closed)";;
esac
n=$(python3 "$WORK/client.py" "$PORT" count)
case "$n" in *2000*) ;; *) fail "after the OOM, a new connection: $n";; esac
sleep 6                                  # one metrics tick
m=$(grep 'metrics:' "$WORK/log" | tail -1 | grep -o 'mem=[0-9]*')
[ "$m" = "mem=0" ] || fail "budget not returned after the connections closed: $m"
stop
echo "OK: --max-memory refused a 2 MB result at a 1 MiB cap and returned the budget ($m)"
exit 0
