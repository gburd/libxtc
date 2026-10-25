#!/bin/bash
#
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# SPDX-License-Identifier: ISC
#
# test/sqlxtc/test_sqlxtc_kill.sh
#	A running sqlxtc server under commit load with one of its storage
#	procs killed (the SQLXTC_TEST_KILL=1 "KILL <proc>" hook):
#	  - bm-provider, bm-trickler: restarted; the server keeps serving,
#	    then SIGTERM drains it in < 1 s with exit 0;
#	  - checkpoint, wal: not restartable; the server stops on its own
#	    (as on SIGTERM) and exits non-zero -- by the end of the 1.5 s
#	    load window after the kill, plus at most 5 s.
#	Every case: a restart on the same store holds every acknowledged
#	commit, and the restarted server's SIGTERM drain is < 1 s.
#
set -u

PORT=${PORT:-15497}
DIR=$(cd "$(dirname "$0")" && pwd)
SVR_BIN=${SQLXTC_SERVER:-$DIR/../../examples/06_sqlxtc/sqlxtc-server}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/sqlxtc-kill.XXXXXX")
SVR_PID=
FAIL=0
RC=255
MS_TAKEN=0

if [ ! -x "$SVR_BIN" ]; then
    echo "SKIP: $SVR_BIN not built"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found"
    exit 0
fi

cleanup() {
    [ -n "$SVR_PID" ] && kill -9 "$SVR_PID" 2>/dev/null
    rm -f "$WORK"/t.db* "$WORK"/s.db* "$WORK"/log "$WORK"/client.py
    rmdir "$WORK" 2>/dev/null || true
}
trap cleanup EXIT

cat > "$WORK/client.py" <<'PY'
import json, socket, sys, threading, time
port, mode = int(sys.argv[1]), sys.argv[2]
def connect():
    for _ in range(100):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=10)
            f = s.makefile("rb"); f.readline()
            return s, f
        except OSError:
            time.sleep(0.1)
    sys.exit("cannot connect")
def q(c, sql, params=None):
    s, f = c
    m = {"q": sql}
    if params is not None: m["params"] = params
    s.sendall((json.dumps(m) + "\n").encode())
    while True:
        line = f.readline()
        if not line: raise EOFError
        r = json.loads(line)
        if "done" in r: return r
        if "err" in r: raise RuntimeError(r["err"])
if mode == "count":
    c = connect()
    s, f = c
    s.sendall(b'{"q":"SELECT count(*) FROM t"}\n')
    while True:
        r = json.loads(f.readline())
        if "row" in r: print(r["row"][0]); break
    sys.exit(0)
victim = sys.argv[3]
c = connect()
q(c, "CREATE TABLE t(k INTEGER PRIMARY KEY, p TEXT)")
acked = []
lock = threading.Lock()
stop = False
def writer(base):
    try:
        w = connect()
        i = 0
        while not stop:
            k = base + i
            q(w, "INSERT INTO t VALUES(?, ?)", [k, "x" * 200])
            with lock: acked.append(k)
            i += 1
    except Exception:
        pass               # the server stopped: what was acked counts
ts = [threading.Thread(target=writer, args=(n * 10**7,)) for n in range(4)]
for t in ts: t.start()
time.sleep(0.5)
try:
    q(c, "KILL " + victim)
    print("killed", victim, file=sys.stderr)
except Exception as e:
    print("kill failed:", e, file=sys.stderr)
    stop = True
    sys.exit(2)
# A second kill finds a live proc only if the first one was restarted.
time.sleep(0.3)
try:
    q(c, "KILL " + victim)
    again = "rekilled"
except Exception:
    again = "gone"
time.sleep(1.2)
stop = True
for t in ts: t.join()
print(len(acked), again)
PY

# start DB: run the server on $WORK/$1 in the background (SVR_PID)
start() {
    SQLXTC_TEST_KILL=1 nohup "$SVR_BIN" -p "$PORT" -t 4 \
        -d "$WORK/$1" < /dev/null > "$WORK/log" 2>&1 &
    SVR_PID=$!
}

# Milliseconds on a monotonic clock (BSD date has no %N).
now_ms() {
    python3 -c 'import time; print(int(time.monotonic() * 1000))'
}

# wait_exit MS: wait up to MS ms for SVR_PID to exit; sets MS_TAKEN to
# the elapsed ms and RC to its exit status (255 if it did not exit)
wait_exit() {
    local t0
    t0=$(now_ms)
    RC=255
    while kill -0 "$SVR_PID" 2>/dev/null; do
        MS_TAKEN=$(( $(now_ms) - t0 ))
        [ "$MS_TAKEN" -ge "$1" ] && return
        sleep 0.02
    done
    wait "$SVR_PID"; RC=$?
    SVR_PID=
    MS_TAKEN=$(( $(now_ms) - t0 ))
}

for victim in bm-provider bm-trickler checkpoint wal; do
    db="s-$victim.db"
    start "$db"
    out=$(python3 "$WORK/client.py" "$PORT" load "$victim" 2>/dev/null)
    acked=${out% *}
    again=${out#* }
    if [ -z "$out" ]; then
        echo "FAIL: $victim: the load client did not finish"
        FAIL=1
        kill -9 "$SVR_PID" 2>/dev/null; SVR_PID=
        continue
    fi
    case $victim in
    bm-provider|bm-trickler)
        if ! kill -0 "$SVR_PID" 2>/dev/null || [ "$again" != rekilled ]; then
            echo "FAIL: $victim: not restarted after its kill ($again)"
            FAIL=1
        fi
        kill -TERM "$SVR_PID" 2>/dev/null
        wait_exit 10000; ms=$MS_TAKEN
        if [ "$RC" -ne 0 ] || [ "$ms" -ge 1000 ]; then
            echo "FAIL: $victim: SIGTERM after the kill: exit $RC in $ms ms (want 0, < 1000)"
            FAIL=1
        fi
        what="restarted (killed twice); SIGTERM drain $ms ms exit $RC"
        ;;
    *)
        wait_exit 5000; ms=$MS_TAKEN
        if [ "$RC" -eq 255 ] || [ "$RC" -eq 0 ]; then
            echo "FAIL: $victim: server exit $RC $ms ms after the kill (want non-zero, < 5000)"
            FAIL=1
            kill -9 "$SVR_PID" 2>/dev/null; SVR_PID=
        fi
        what="server stopped itself, exit $RC"
        ;;
    esac
    grep -q "storage proc $victim exited" "$WORK/log" && what="$what (logged)"
    start "$db"
    rows=$(python3 "$WORK/client.py" "$PORT" count 2>/dev/null)
    kill -TERM "$SVR_PID" 2>/dev/null
    wait_exit 10000; ms2=$MS_TAKEN
    # >= acked: an insert can commit with its ack lost to the stop
    if [ -z "$rows" ]; then
        echo "FAIL: $victim: restart did not answer"
        FAIL=1
    elif [ "$rows" -lt "$acked" ]; then
        echo "FAIL: $victim: restart has $rows rows, $acked were acknowledged"
        FAIL=1
    fi
    if [ "$RC" -ne 0 ] || [ "$ms2" -ge 1000 ]; then
        echo "FAIL: $victim: restarted server's SIGTERM: exit $RC in $ms2 ms"
        FAIL=1
    fi
    echo "  $victim: $what; acked $acked, after restart $rows rows"
    rm -f "$WORK/$db"*
done

if [ "$FAIL" -ne 0 ]; then
    tail -5 "$WORK/log"
    echo "FAIL: test_sqlxtc_kill"
    exit 1
fi
echo "OK: killed storage procs are restarted or stop the server cleanly"
exit 0
