#!/bin/sh
# test/tools/test_gdb_debuginfo.sh
#
#	Gate the debug-info probe in tools/gdb/xtc-gdb.py.
#
#	Reported 2026-09-14: against a stripped/release libxtc the state
#	commands printed an empty-but-successful census ("no loops registered",
#	"0 parked") -- indistinguishable from a healthy true negative, which
#	for a strand-diagnosis tool is the worst failure mode.  A consumer lost
#	hours concluding "the fiber layer is not involved" when the tool simply
#	could not see.  The fix makes the blind case LOUD: the commands
#	hard-error and refuse to print a census when libxtc's debug info is
#	missing, and xtc-check reports per-capability what resolves.
#
#	This gate proves BOTH directions on the same workload:
#	  - a DEBUG-built program: xtc-loops prints a real census and xtc-check
#	    reports the capabilities OK;
#	  - a STRIPPED copy of that same program: xtc-loops REFUSES with the
#	    distinct "debug info missing" error, and does NOT print an empty
#	    census, and xtc-check reports NO DEBUG INFO.
#
#	SKIPs where the environment cannot support it (no gdb, no ptrace,
#	no static lib, or the link fails).

set -e

: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set}"
: "${XTC_BUILD_DIR:?XTC_BUILD_DIR must be set}"

GDBPY="$XTC_SRC_DIR/tools/gdb/xtc-gdb.py"
LIB="$XTC_BUILD_DIR/libxtc.a"
TMPD=$(mktemp -d)

# The EXIT trap must never change the exit status: under set -e a failing
# `[ -n "$PID" ] && kill` (the pid is cleared or already dead) would REPLACE
# the script's status -- an `exit 0` became rc=1 (same bug as gdb-cqes).
cleanup() {
	_rc=$?
	set +e
	[ -n "$PID_DBG" ] && kill -9 "$PID_DBG" 2>/dev/null
	[ -n "$PID_STR" ] && kill -9 "$PID_STR" 2>/dev/null
	find "$TMPD" -mindepth 1 -delete 2>/dev/null
	rmdir "$TMPD" 2>/dev/null
	exit "$_rc"
}
trap cleanup 0 1 2 15

if ! command -v gdb >/dev/null 2>&1; then
	echo "  [gdb-debuginfo] SKIP: gdb not on PATH"
	exit 0
fi
if [ ! -f "$GDBPY" ] || [ ! -f "$LIB" ]; then
	echo "  [gdb-debuginfo] SKIP: script or library not built"
	exit 0
fi
if [ -r /proc/sys/kernel/yama/ptrace_scope ] &&
    [ "$(cat /proc/sys/kernel/yama/ptrace_scope)" != "0" ]; then
	echo "  [gdb-debuginfo] SKIP: ptrace_scope != 0, cannot attach"
	exit 0
fi

cat > "$TMPD/prog.c" <<'PROGEOF'
/* A tiny N-loop executor with parked fibers, SIGSTOP'd so gdb can attach
 * to a frozen, inspectable state. */
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include "xtc.h"
#include "xtc_proc.h"

static void worker(void *a) { (void)a; for (;;) xtc_proc_sleep(50000000); }
static void *stopper(void *a) { (void)a; sleep(2); raise(SIGSTOP); return 0; }

int
main(void)
{
	xtc_exec_t *e = NULL;
	xtc_proc_opts_t o;
	pthread_t th;
	int i;
	memset(&o, 0, sizeof o);
	o.migratable = 1;
	if (xtc_exec_init(&e, 3) != XTC_OK) return 1;
	if (pthread_create(&th, NULL, stopper, NULL) != 0) return 1;
	pthread_detach(th);
	for (i = 0; i < 3; i++) {
		xtc_pid_t p;
		(void)xtc_proc_spawn(xtc_exec_loop(e, 0), worker, NULL, &o, &p);
	}
	(void)xtc_exec_run(e);
	return 0;
}
PROGEOF

# Build with debug info; the library is linked in, so its own -g state is
# what matters -- $LIB carries the build's debug info (or not).
if ! ${CC:-cc} -O1 -g -w -I "$XTC_SRC_DIR/src/inc" -I "$XTC_BUILD_DIR" \
    -o "$TMPD/prog_dbg" "$TMPD/prog.c" "$LIB" \
    -pthread -luring -lssl -lcrypto -ldl -lm > "$TMPD/cc.log" 2>&1; then
	echo "  [gdb-debuginfo] SKIP: test program will not link"
	exit 0
fi
# A stripped copy: no symbols/debug info at all -> the tool must be blind.
cp "$TMPD/prog_dbg" "$TMPD/prog_str"
strip "$TMPD/prog_str" 2>/dev/null || {
	echo "  [gdb-debuginfo] SKIP: strip unavailable"
	exit 0
}

wait_stopped() {
	_p=$1 _i=0
	while [ "$_i" -lt 100 ]; do
		_st=$(awk '{print $3}' "/proc/$_p/stat" 2>/dev/null || echo "")
		[ "$_st" = "T" ] && return 0
		_i=$((_i + 1))
		sleep 0.1
	done
	return 1
}

fail=0

# --- STRIPPED: must refuse loudly, must NOT print an empty census --------
"$TMPD/prog_str" &
PID_STR=$!
if ! wait_stopped "$PID_STR"; then
	echo "  [gdb-debuginfo] SKIP: stripped program never stopped"
	exit 0
fi
gdb -q -p "$PID_STR" -batch \
    -ex "source $GDBPY" \
    -ex "xtc-loops" \
    -ex "xtc-check" > "$TMPD/str.txt" 2>&1 || true
kill -9 "$PID_STR" 2>/dev/null; PID_STR=

if ! grep -qi "debug info missing" "$TMPD/str.txt"; then
	echo "  [gdb-debuginfo] FAIL: stripped build did not report 'debug info missing'"
	fail=1
fi
# The census must NOT have printed a (misleading) empty table.
if grep -q "no loops registered" "$TMPD/str.txt"; then
	echo "  [gdb-debuginfo] FAIL: stripped build printed an empty census"
	echo "             ('no loops registered') instead of refusing"
	fail=1
fi
if ! grep -q "NO DEBUG INFO" "$TMPD/str.txt"; then
	echo "  [gdb-debuginfo] FAIL: xtc-check did not report NO DEBUG INFO on a stripped build"
	fail=1
fi

# --- DEBUG: must print a real census and xtc-check must report OK --------
"$TMPD/prog_dbg" &
PID_DBG=$!
if ! wait_stopped "$PID_DBG"; then
	echo "  [gdb-debuginfo] SKIP: debug program never stopped"
	exit 0
fi
gdb -q -p "$PID_DBG" -batch \
    -ex "source $GDBPY" \
    -ex "xtc-check" \
    -ex "xtc-loops" > "$TMPD/dbg.txt" 2>&1 || true
kill -9 "$PID_DBG" 2>/dev/null; PID_DBG=

# If this build itself has no debug info (a release make check), the debug
# arm is not applicable -- SKIP rather than fail, but only if the stripped
# arm already proved the refusal path.
if grep -qi "debug info missing" "$TMPD/dbg.txt"; then
	if [ "$fail" -eq 0 ]; then
		echo "  [gdb-debuginfo] OK (refusal path only): this build has no"
		echo "          debug info, so only the stripped/refuse arm ran"
		echo "          -- and it correctly refused."
		exit 0
	fi
else
	if ! grep -q "loop 0x" "$TMPD/dbg.txt"; then
		echo "  [gdb-debuginfo] FAIL: debug build did not print a loop census"
		fail=1
	fi
	if ! grep -q "trustworthy" "$TMPD/dbg.txt"; then
		echo "  [gdb-debuginfo] FAIL: xtc-check did not report trustworthy on a debug build"
		fail=1
	fi
fi

if [ "$fail" -ne 0 ]; then
	echo "--- stripped output ---"; sed 's/^/    /' "$TMPD/str.txt"
	echo "--- debug output ---";    sed 's/^/    /' "$TMPD/dbg.txt"
	exit 1
fi

echo "  [gdb-debuginfo] OK: stripped libxtc refuses loudly (no empty census),"
echo "          debug libxtc prints a real census; xtc-check distinguishes them"
exit 0
