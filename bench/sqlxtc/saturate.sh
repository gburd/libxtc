#!/bin/bash
# bench/sqlxtc/saturate.sh -- concurrent saturation bench.
#
# Usage:
#   ./saturate.sh [n_clients] [n_queries] [port]
#
# Reports CPU%, RSS plateau, queries/sec, rejected count.
set -u

N_CLIENTS=${1:-200}
N_QUERIES=${2:-1000}
PORT=${3:-15436}

DIR=$(dirname "$(readlink -f "$0")")
SVR=$DIR/../../examples/06_sqlxtc/sqlxtc-server
DBFILE=/tmp/sqlxtc-bench.db
LOGFILE=/tmp/sqlxtc-bench.log
PIDFILE=/tmp/sqlxtc-bench.pid
RESULTS=$DIR/RESULTS.md

if [ ! -x "$SVR" ]; then
    echo "FAIL: $SVR not built"; exit 1
fi

CORES=${CORES:-4}
MAX_MEM=${MAX_MEM:-$((1024*1024*1024))}
MAX_CLIENTS=${MAX_CLIENTS:-$((N_CLIENTS+8))}

pkill -9 -f "sqlxtc-server.*-p $PORT" 2>/dev/null || true
rm -f "$DBFILE" "$DBFILE-shm" "$DBFILE-wal" "$LOGFILE" "$PIDFILE"

echo "=== Starting sqlxtc-server ==="
echo "  --cores=$CORES --max-memory=$MAX_MEM --max-clients=$MAX_CLIENTS"
nohup setsid "$SVR" --port="$PORT" --db="$DBFILE" \
    --cores="$CORES" --max-memory="$MAX_MEM" \
    --max-clients="$MAX_CLIENTS" \
    < /dev/null > "$LOGFILE" 2>&1 &
disown
sleep 0.5

SVR_PID=$(pgrep -f "sqlxtc-server.*--port=$PORT" | head -1)
[ -n "$SVR_PID" ] || { echo "server did not start"; cat "$LOGFILE"; exit 1; }
echo "$SVR_PID" > "$PIDFILE"

cleanup() {
    kill -9 "$SVR_PID" 2>/dev/null || true
    rm -f "$DBFILE" "$DBFILE-shm" "$DBFILE-wal" "$PIDFILE"
}
trap cleanup EXIT

echo "Server pid: $SVR_PID"

echo
echo "=== Pre-creating schema ==="
python3 - "$PORT" <<'PY'
import socket, json, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=5)
f = s.makefile('rwb', buffering=0)
f.readline()
for q in ["DROP TABLE IF EXISTS t",
          "CREATE TABLE t(id INTEGER PRIMARY KEY, k INTEGER, v TEXT)"]:
    f.write((json.dumps({"q":q})+"\n").encode())
    while True:
        line = f.readline()
        m = json.loads(line)
        if 'done' in m or 'err' in m: break
s.close()
PY

# Sample CPU/RSS during the bench.
SAMPLES=/tmp/sqlxtc-samples.txt
> "$SAMPLES"
( while kill -0 "$SVR_PID" 2>/dev/null; do
    ps -p "$SVR_PID" -o pcpu,pmem,rss,nlwp 2>/dev/null | tail -1 >> "$SAMPLES"
    sleep 0.5
  done ) &
SAMPLER_PID=$!

echo
echo "=== Driving $N_CLIENTS clients * $N_QUERIES queries ==="
T0=$(date +%s.%N)
python3 "$DIR/saturate_client.py" "$PORT" "$N_CLIENTS" "$N_QUERIES" \
    > /tmp/sqlxtc-bench-out.txt 2>&1
RC=$?
T1=$(date +%s.%N)

# Stop the sampler.
kill "$SAMPLER_PID" 2>/dev/null || true
wait 2>/dev/null

ELAPSED=$(awk "BEGIN{print $T1-$T0}")

# Parse client output.
TOTAL=$(grep -E "^total " /tmp/sqlxtc-bench-out.txt | awk '{print $2}')
ERRORS=$(grep -E "^errors " /tmp/sqlxtc-bench-out.txt | awk '{print $2}')
REJECTED=$(grep -E "^rejected " /tmp/sqlxtc-bench-out.txt | awk '{print $2}')

[ -n "$TOTAL" ] || TOTAL=0
QPS=$(awk "BEGIN{if ($ELAPSED>0) printf \"%.0f\", $TOTAL/$ELAPSED; else print 0}")

# CPU/RSS plateau: take peak across samples.
CPU_PEAK=$(awk '{print $1}' "$SAMPLES" | sort -n | tail -1)
RSS_PEAK=$(awk '{print $3}' "$SAMPLES" | sort -n | tail -1)
RSS_MB=$(awk "BEGIN{printf \"%.1f\", $RSS_PEAK/1024}")
THREADS_PEAK=$(awk '{print $4}' "$SAMPLES" | sort -n | tail -1)

echo
echo "=== Results ==="
echo "  elapsed:    ${ELAPSED}s"
echo "  total:      $TOTAL queries"
echo "  qps:        $QPS"
echo "  errors:     $ERRORS"
echo "  rejected:   $REJECTED"
echo "  cpu peak:   ${CPU_PEAK}%   (sched_setaffinity to $CORES cores; ${CORES}00% would saturate)"
echo "  rss peak:   ${RSS_MB} MiB  (cap: $((MAX_MEM/1024/1024)) MiB)"
echo "  threads:    $THREADS_PEAK"

# Append to RESULTS.md.
{
    echo
    echo "## Run at $(date -Iseconds)"
    echo
    echo "Config: --cores=$CORES --max-memory=$MAX_MEM --max-clients=$MAX_CLIENTS"
    echo
    echo "| metric        | value                                      |"
    echo "|---------------|--------------------------------------------|"
    echo "| clients       | $N_CLIENTS                                 |"
    echo "| queries/cli   | $N_QUERIES                                 |"
    echo "| total queries | $TOTAL                                     |"
    echo "| elapsed       | ${ELAPSED}s                                |"
    echo "| qps           | $QPS                                       |"
    echo "| cpu peak      | ${CPU_PEAK}% (cap: ${CORES}00%)            |"
    echo "| rss peak      | ${RSS_MB} MiB (cap: $((MAX_MEM/1024/1024)) MiB) |"
    echo "| threads       | $THREADS_PEAK                              |"
    echo "| errors        | $ERRORS                                    |"
    echo "| rejected      | $REJECTED                                  |"
} >> "$RESULTS"

if [ "$RC" -ne 0 ]; then
    echo
    echo "FAIL: client exit $RC"
    cat /tmp/sqlxtc-bench-out.txt
    exit 1
fi
echo
echo "OK: saturation bench completed"
exit 0
