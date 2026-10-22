#!/bin/bash
# test/sqlxtc/test_sqlxtc_conn.sh
#   Two connection-handling bugs, both in conn.c's request loop:
#   1. Pipelined requests stalled.  A pass stops running requests once
#      256 KiB of output is pending; when the socket then took all of
#      it, the proc parked for READABLE with complete requests still
#      buffered -- the client had sent them all, so only the park's 1 s
#      timeout woke it: about 1 s per 256 KiB of pipelined output.
#   2. A half-close truncated the result.  recv() == 0 (the client shut
#      its write side, as `nc -N` and scripted clients do) marked the
#      connection dead, and the final flush skips a dead connection, so
#      a result still in the write buffer was dropped: no "done", no
#      error, just a short stream.
#   Asserts 20 pipelined results arrive in under 1 s, and a 10 MB result
#   requested just before a half-close arrives whole (every row, then
#   "done") even when the client starts reading late.
#
#   Portable; no setsid, no `timeout`.  Kills by explicit PID only.
set -u

PORT=${PORT:-15495}
DIR=$(cd "$(dirname "$0")" && pwd)
SVR_BIN=${SQLXTC_SERVER:-$DIR/../../examples/06_sqlxtc/sqlxtc-server}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/sqlxtc-conn.XXXXXX")
SVR_PID=

if [ ! -x "$SVR_BIN" ]; then
    echo "SKIP: $SVR_BIN not built"
    exit 0
fi

cleanup() {
    [ -n "$SVR_PID" ] && kill -9 "$SVR_PID" 2>/dev/null
    rm -f "$WORK"/t.db* "$WORK"/log "$WORK"/client.py
    rmdir "$WORK" 2>/dev/null || true
}
trap cleanup EXIT

nohup "$SVR_BIN" -p "$PORT" -d "$WORK/t.db" < /dev/null > "$WORK/log" 2>&1 &
SVR_PID=$!

cat > "$WORK/client.py" <<'PY'
import json, socket, sys, time
port = int(sys.argv[1])
def connect():
    for _ in range(100):
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=60)
        except OSError:
            time.sleep(0.1)
    sys.exit("cannot connect")
# Buffered reads: an unbuffered readline is one recv per byte, which
# made this client, not the server, the bottleneck.
s = connect(); f = s.makefile("rb"); f.readline()
def q(sql, params=None):
    m = {"q": sql}
    if params: m["params"] = params
    s.sendall((json.dumps(m) + "\n").encode())
    while True:
        r = json.loads(f.readline())
        if "done" in r or "err" in r: return r
q("CREATE TABLE p(k INTEGER PRIMARY KEY, pad TEXT)")
q("BEGIN")
for i in range(5000):
    q("INSERT INTO p VALUES(?, ?)", [i, "x" * 2000])
q("COMMIT")
bad = 0
# 1. 20 pipelined SELECTs of 200 rows x 2000 B (~400 KB each), one send.
t0 = time.time()
s.sendall(b'{"q":"SELECT * FROM p WHERE k < 200"}\n' * 20)
got = rows = 0
while got < 20:
    line = f.readline()
    if not line: sys.exit("EOF in the pipeline")
    if line.startswith(b'{"row"'): rows += 1
    elif line.startswith(b'{"done"') or line.startswith(b'{"err"'): got += 1
ms = (time.time() - t0) * 1e3
print(f"  pipeline: 20 results, {rows} rows in {ms:.0f} ms")
if rows != 4000 or ms >= 1000:
    print("FAIL: 20 pipelined results took %.0f ms (want < 1000), %d rows" % (ms, rows)); bad = 1
s.close()
# 2. A ~10 MB result, then a half-close; start reading 300 ms later.
s = connect()
s.recv(1 << 10)                       # the banner (one short line)
s.sendall(b'{"q":"SELECT * FROM p"}\n'); s.shutdown(socket.SHUT_WR)
time.sleep(0.3)
data = bytearray()
while True:
    b = s.recv(1 << 16)
    if not b: break
    data += b
lines = bytes(data).split(b"\n")
nrow = sum(1 for l in lines if l.startswith(b'{"row"'))
ndone = sum(1 for l in lines if l == b'{"done":5000}')
print(f"  half-close: {nrow} rows, done={ndone}, {len(data)} bytes")
if nrow != 5000 or ndone != 1:
    print(f"FAIL: after a half-close the result arrived truncated ({nrow} of 5000 rows, done={ndone})"); bad = 1
sys.exit(bad)
PY
python3 "$WORK/client.py" "$PORT"
rc=$?
kill -TERM "$SVR_PID" 2>/dev/null
t=0
while kill -0 "$SVR_PID" 2>/dev/null && [ $t -lt 100 ]; do sleep 0.1; t=$((t + 1)); done
[ $rc -eq 0 ] || exit 1
echo "OK: pipelined requests run without stalling; a half-closed connection gets its whole result"
exit 0
