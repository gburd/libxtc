#!/bin/bash
# test/sqlxtc/test_sqlxtc_iosoak.sh
#   Concurrent network + storage soak for sqlxtc-server: one loop per CPU,
#   a durable store whose working set outgrows its buffer pool (so page
#   eviction, write-back and page reads from disk run alongside the WAL's
#   fsyncs), and CLIENTS TCP clients writing 1-3.9 KiB rows, updating and
#   reading them back for SECS seconds, every answer checked against each
#   client's model, then the whole table read back and compared.  Rows
#   are 1-3.9 KiB (sqlxtc's row limit is 4 KiB).
#
#   Env: CLIENTS (32) SECS (15) FRAMES (512 pages = 2 MiB pool)
#        PORT (11000 + pid % 20000; another on a collision unless set)
#        TMPDIR
#        SOAK_WRAP: a command prefix for the server (the io_uring gate
#        runs it as SOAK_WRAP="uringscope -f --json=r.json --").
#   Portable (Linux + macOS); never uses setsid or `timeout`.
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
SVR_BIN=${SQLXTC_SERVER:-$DIR/../../examples/06_sqlxtc/sqlxtc-server}
CLIENTS=${CLIENTS:-32}
SECS=${SECS:-15}
FRAMES=${FRAMES:-512}
# Below the ephemeral ranges (Linux 32768-60999, macOS 49152-): an
# outgoing connection holding the port fails the listen (21000 +
# pid % 20000 reached 40999).  FreeBSD's range starts at 10000, so a
# collision still moves to another port.
FIXED_PORT=${PORT:+1}
PORT=${PORT:-$((11000 + $$ % 20000))}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/sqlxtc-iosoak.XXXXXX")
DBFILE=$WORK/soak.db
LOGFILE=$WORK/server.log

if [ ! -x "$SVR_BIN" ]; then
	echo "SKIP: $SVR_BIN not built"
	rmdir "$WORK" 2>/dev/null
	exit 0
fi
if command -v nproc > /dev/null 2>&1; then
	CORES=$(nproc)
else
	CORES=$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)
fi

# The server process: WRAP_PID itself, or under a SOAK_WRAP wrapper a
# descendant of it (pgrep -P works on Linux, macOS and the BSDs), found
# by the name of the binary actually run (SQLXTC_SERVER may be a -plant
# build), cut to 15 characters as Linux's comm is.
SVR_NAME=$(basename "$SVR_BIN" | cut -c1-15)
WRAP_PID=
WD_PID=
server_pid() {
	p=$WRAP_PID
	while [ -n "$p" ]; do
		c=$(ps -o comm= -p "$p" 2>/dev/null)
		[ "$(basename "${c:-/}" | cut -c1-15)" = "$SVR_NAME" ] && break
		p=$(pgrep -P "$p" 2>/dev/null | head -1)
	done
	echo "$p"
}

cleanup() {
	# every exit path, a failed one too, stops the server it started
	sp=$(server_pid)
	[ -n "$sp" ] && kill -TERM "$sp" 2>/dev/null
	[ -n "$WRAP_PID" ] && wait "$WRAP_PID" 2>/dev/null
	[ -n "$WD_PID" ] && kill "$WD_PID" 2>/dev/null
	[ -n "$WD_PID" ] && wait "$WD_PID" 2>/dev/null
	find "$WORK" -mindepth 1 -delete 2>/dev/null
	rmdir "$WORK" 2>/dev/null
}
trap cleanup EXIT

# Start the server on $PORT with a new store and wait (up to 10 s) for
# its listener.  1: the port is taken (something answers there, or the
# server exited without listening); 2: it never came up.
start_server() {
	find "$WORK" -mindepth 1 -delete 2>/dev/null
	# A listener already there would answer this run's clients.
	if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
		return 1
	fi
	# shellcheck disable=SC2086  # SOAK_WRAP is a word list
	${SOAK_WRAP:-} "$SVR_BIN" -p "$PORT" -d "$DBFILE" -t "$CORES" -n 256 \
	    --storage-frames="$FRAMES" < /dev/null > "$LOGFILE" 2>&1 &
	WRAP_PID=$!
	i=0
	until (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; do
		if ! kill -0 "$WRAP_PID" 2>/dev/null; then
			wait "$WRAP_PID" 2>/dev/null
			WRAP_PID=
			return 1
		fi
		i=$((i + 1))
		[ "$i" -ge 100 ] && return 2
		sleep 0.1
	done
	return 0
}
tries=0
until start_server; do
	st=$?
	tries=$((tries + 1))
	if [ "$st" -eq 2 ] || [ -n "$FIXED_PORT" ] || [ "$tries" -ge 5 ]; then
		echo "FAIL: sqlxtc-server did not start on port $PORT" \
		    "(attempt $tries; a port in use?)"
		cat "$LOGFILE" 2>/dev/null
		exit 1
	fi
	n=$((11000 + (PORT - 11000 + 7919) % 20000))
	echo "note: port $PORT is in use; trying $n"
	PORT=$n
done

# The watchdog polls, so it exits with the server, and keeps no stdout:
# a lone `sleep` holding it would stall a reader of this script (a CI
# step log, `| tail`) for the full bound after the run ended.
( n=$((SECS + 120))
  while [ "$n" -gt 0 ] && kill -0 "$WRAP_PID" 2>/dev/null; do
	sleep 1
	n=$((n - 1))
  done
  [ "$n" -eq 0 ] && kill -9 "$WRAP_PID" ) > /dev/null 2>&1 &
WD_PID=$!

python3 "$DIR/test_sqlxtc_iosoak.py" "$PORT" "$CLIENTS" "$SECS"
ST=$?
STORE=$(du -sk "$DBFILE".xstore 2>/dev/null | cut -f1)
LOGKB=$(du -sk "$DBFILE".xstore-wal 2>/dev/null | cut -f1)

# Stop the server (SIGTERM drains) and let any wrapper finish its report.
SPID=$(server_pid)
[ -n "$SPID" ] && kill -TERM "$SPID" 2>/dev/null
wait "$WRAP_PID" 2>/dev/null

if [ "$ST" -ne 0 ]; then
	echo "FAIL: soak client reported errors (status $ST)"
	tail -20 "$LOGFILE"
	exit 1
fi
POOL_KB=$((FRAMES * 4))
if [ "${STORE:-0}" -le "$POOL_KB" ]; then
	echo "FAIL: the store (${STORE:-0} KiB) never outgrew the" \
	    "${POOL_KB} KiB buffer pool: no page eviction was exercised"
	exit 1
fi
echo "OK: sqlxtc io soak, $CLIENTS clients for ${SECS}s at -t $CORES;" \
    "store ${STORE} KiB vs a ${POOL_KB} KiB pool, log ${LOGKB} KiB"
