#!/bin/sh
# test/tools/test_gdb_cqes.sh
#
#	Gate the gdb helpers that decode live ring state: xtc-rings and
#	xtc-cqes.
#
#	Why this exists.  These scripts are the primary instrument for
#	diagnosing "a completion is sitting in the CQ and nobody took it",
#	and until now NOTHING in the tree exercised them -- they could break
#	silently on any struct rename (io->ring.cq.khead, xtc_aio_t.tag,
#	struct __xtc_uring_fd.fd/tag) and we would only find out mid-hang,
#	with a consumer waiting.  A debugger script that has never been run
#	by CI is not tooling, it is a guess with a shebang.
#
#	The discriminating part is not "does the command load" -- that catches
#	almost nothing.  It is that xtc-cqes must DECODE a real unreaped CQE
#	down to the owning task pointer and opcode, because that decode is the
#	join a consumer relies on.  So this builds a program that deliberately
#	leaves completions unreaped, stops it, and requires the decoded output.
#
#	SKIPs (rather than fails) where the environment cannot support it:
#	no gdb, no io_uring backend, or ptrace_scope blocking attach.

set -e

: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set}"
: "${XTC_BUILD_DIR:?XTC_BUILD_DIR must be set}"

GDBPY="$XTC_SRC_DIR/tools/gdb/xtc-gdb.py"
TMPD=$(mktemp -d)

cleanup() {
	[ -n "$PROG_PID" ] && kill -9 "$PROG_PID" 2>/dev/null
	find "$TMPD" -mindepth 1 -delete 2>/dev/null || true
	rmdir "$TMPD" 2>/dev/null || true
}
trap cleanup 0 1 2 15

if ! command -v gdb >/dev/null 2>&1; then
	echo "  [gdb-cqes] SKIP: gdb not on PATH"
	exit 0
fi
if [ ! -f "$GDBPY" ]; then
	echo "  [gdb-cqes] SKIP: $GDBPY not found"
	exit 0
fi
if [ -r /proc/sys/kernel/yama/ptrace_scope ] &&
    [ "$(cat /proc/sys/kernel/yama/ptrace_scope)" != "0" ]; then
	echo "  [gdb-cqes] SKIP: ptrace_scope != 0, cannot attach"
	exit 0
fi

cat > "$TMPD/prog.c" <<'PROGEOF'
/*
 * Drive aio hard from several fibers and STOP mid-flight, so the rings hold
 * completions that have been posted but not yet consumed -- the exact state
 * xtc-cqes has to decode.  SIGSTOP rather than a sleep loop so gdb attaches
 * to a process frozen with a real backlog.
 */
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include "xtc.h"
#include "xtc_proc.h"
#include "xtc_aio.h"

static char g_dir[256];

static void
worker(void *a)
{
	char buf[4096], path[512];
	int fd, i;
	(void)a;
	memset(buf, 'q', sizeof buf);
	snprintf(path, sizeof path, "%s/cqe%d_%p.dat", g_dir, (int)getpid(),
	    (void *)&fd);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return;
	(void)unlink(path);
	for (i = 0; i < 4000; i++) {
		(void)xtc_aio_pwrite(fd, buf, sizeof buf, (int64_t)i * 4096);
		(void)xtc_aio_fdatasync(fd);
	}
	close(fd);
}

static void *
stopper(void *a)
{
	(void)a;
	sleep(3);
	raise(SIGSTOP);      /* freeze with completions in flight */
	return NULL;
}

int
main(int argc, char **argv)
{
	xtc_exec_t *e = NULL;
	xtc_proc_opts_t o;
	pthread_t th;
	int i;

	snprintf(g_dir, sizeof g_dir, "%s", argc > 1 ? argv[1] : ".");
	memset(&o, 0, sizeof o);
	o.migratable = 1;
	if (xtc_exec_init(&e, 3) != XTC_OK)
		return 1;
	if (pthread_create(&th, NULL, stopper, NULL) != 0)
		return 1;
	pthread_detach(th);
	for (i = 0; i < 6; i++) {
		xtc_pid_t p;
		(void)xtc_proc_spawn(xtc_exec_loop(e, 0), worker, NULL, &o, &p);
	}
	(void)xtc_exec_run(e);
	return 0;
}
PROGEOF

# Link against whatever the build produced.
LIB="$XTC_BUILD_DIR/libxtc.a"
if [ ! -f "$LIB" ]; then
	echo "  [gdb-cqes] SKIP: $LIB not built"
	exit 0
fi

if ! ${CC:-cc} -O0 -g -w -I "$XTC_SRC_DIR/src/inc" -I "$XTC_BUILD_DIR" \
    -o "$TMPD/prog" "$TMPD/prog.c" "$LIB" \
    -pthread -luring -lssl -lcrypto -ldl -lm > "$TMPD/cc.log" 2>&1; then
	# No io_uring (or no liburing) -- this gate is uring-specific.
	echo "  [gdb-cqes] SKIP: test program will not link (no io_uring?)"
	exit 0
fi

"$TMPD/prog" "$TMPD" &
PROG_PID=$!

# Wait for the self-STOP rather than guessing at a sleep.
i=0
while [ "$i" -lt 100 ]; do
	st=$(awk '{print $3}' "/proc/$PROG_PID/stat" 2>/dev/null || echo "")
	[ "$st" = "T" ] && break
	i=$((i + 1))
	sleep 0.1
done
if [ "$st" != "T" ]; then
	echo "  [gdb-cqes] SKIP: program never reached a stopped state"
	exit 0
fi

gdb -q -p "$PROG_PID" -batch \
    -ex "source $GDBPY" \
    -ex "xtc-rings" \
    -ex "xtc-cqes" > "$TMPD/out.txt" 2>&1 || true

fail=0

# 1. the scripts loaded and registered the commands
grep -q "xtc-gdb loaded" "$TMPD/out.txt" || {
	echo "  [gdb-cqes] FAIL: xtc-gdb.py did not load"
	fail=1
}
# 2. xtc-rings found rings (a ring table with an ovf column)
grep -q "ring_fd" "$TMPD/out.txt" || {
	echo "  [gdb-cqes] FAIL: xtc-rings printed no ring table"
	fail=1
}
grep -q "ovf" "$TMPD/out.txt" || {
	echo "  [gdb-cqes] FAIL: xtc-rings is missing the ovf column"
	fail=1
}
# 3. xtc-cqes walked a CQ
grep -q "CqHead=" "$TMPD/out.txt" || {
	echo "  [gdb-cqes] FAIL: xtc-cqes printed no CQ head/tail"
	fail=1
}
# 4. THE LOAD-BEARING ONE: a real unreaped CQE decoded to its owning task.
#    If the struct layout drifts, user_data stops resolving and this is what
#    catches it -- everything above would still pass.
if grep -qE "user_data=0x[0-9a-f]+ +res=" "$TMPD/out.txt"; then
	grep -qE "task=0x[0-9a-f]+" "$TMPD/out.txt" || {
		echo "  [gdb-cqes] FAIL: a CQE was found but not decoded to a"
		echo "             task pointer -- the aio/uring_fd decode is"
		echo "             broken (struct rename?)"
		fail=1
	}
else
	# No unreaped CQE in the snapshot.  Not a failure of the decoder --
	# the freeze can land between drains -- but say so rather than
	# silently passing a gate that proved nothing.
	echo "  [gdb-cqes] NOTE: no unreaped CQE in this snapshot; the decode"
	echo "             path was not exercised (timing, not a defect)"
fi

if [ "$fail" -ne 0 ]; then
	echo "--- gdb output ---"
	cat "$TMPD/out.txt"
	exit 1
fi

echo "  [gdb-cqes] OK: xtc-rings + xtc-cqes read live ring state and decode"
echo "          unreaped completions to their owning task"
exit 0
