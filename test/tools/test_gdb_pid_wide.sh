#!/bin/sh
# test/tools/test_gdb_pid_wide.sh
#
#	Gate the pid decoders against the 2.0 pid (16 bytes: u32 loop_id,
#	u32 local_id, u64 gen) at a slot the 1.x layout could not hold.
#
#	A program spawns 70,000 parked procs on ONE loop (slot 69,999 is past
#	the old 16-bit ceiling of 65,536) with the tail ring on, then stops
#	itself.  gdb attaches and runs xtc-procs and xtc-tail-dump; then
#	tools/xtc-tail.py reads the dump.  Required:
#	  - xtc-procs lists the pid 0.69999.1 (the walk covers the grown
#	    table and _pid_str prints the wide slot);
#	  - the dump is an XTCL version 3 trace (the 16-byte pid), and
#	    xtc-tail.py shows SPAWN for 0.69999.1.
#	lldb's xtc-procs must list the same pid and count (it listed NO proc
#	at all before 2.0: lldb cannot Dereference an _Atomic(T *), and the
#	loop registry's fields are _Atomic).
#	Plus a reader-only check that needs no gdb: a synthesized v3 trace
#	with a 64-bit gen and the v3 loop sentinel, and a v2 trace with the
#	1.x sentinel (0xFFFF), both decode, and an unknown version is refused.
#
#	SKIPs where the environment cannot support the live part: no python3,
#	no gdb, ptrace_scope != 0, a library without debug info, or a host
#	whose vm.max_map_count cannot hold 70,000 fiber stacks.

set -e

: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set}"
: "${XTC_BUILD_DIR:?XTC_BUILD_DIR must be set}"

GDBPY="$XTC_SRC_DIR/tools/gdb/xtc-gdb.py"
VIEWER="$XTC_SRC_DIR/tools/xtc-tail.py"
TMPD=$(mktemp -d)
PROG_PID=

cleanup() {
	_rc=$?
	set +e
	[ -n "$PROG_PID" ] && kill -9 "$PROG_PID" 2>/dev/null
	find "$TMPD" -mindepth 1 -delete 2>/dev/null
	rmdir "$TMPD" 2>/dev/null
	exit "$_rc"
}
trap cleanup 0 1 2 15

PY=$(command -v python3 2>/dev/null || true)
if [ -z "$PY" ]; then
	echo "  [pid-wide] SKIP: python3 not available"
	exit 0
fi

# ---- 1. reader only: v3 wide values, v2 sentinel, unknown version ------
"$PY" - "$TMPD" <<'PYEOF'
import struct, sys
d = sys.argv[1]

def leb(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        out.append(b | 0x80 if v else b)
        if not v:
            return bytes(out)

def trace(path, ver, evs):
    blob = bytearray(struct.pack("<IIII", 0x5854434C, ver, 1, len(evs)))
    blob += struct.pack("<Q", 1000)
    for kind, loop, local, gen in evs:
        blob += bytes([kind, 0]) + leb(10) + leb(loop) + leb(local) \
            + leb(gen) + leb(0)
    open(path, "wb").write(blob)

SPAWN, LOOP_POLL = 0, 8
trace(d + "/v3.xtcl", 3, [(SPAWN, 7, 70000, 2**40 + 5),
                          (LOOP_POLL, 0xFFFFFFFF, 0, 0)])
trace(d + "/v2.xtcl", 2, [(SPAWN, 1, 3, 4), (LOOP_POLL, 0xFFFF, 0, 0)])
trace(d + "/v9.xtcl", 9, [(SPAWN, 1, 3, 4)])
PYEOF
out3=$("$PY" "$VIEWER" "$TMPD/v3.xtcl")
out2=$("$PY" "$VIEWER" "$TMPD/v2.xtcl")
fail=0
echo "$out3" | grep -q "pid=7.70000.1099511627781" ||
    { echo "  [pid-wide] FAIL: v3 wide pid not decoded"; fail=1; }
echo "$out3" | grep -q "pid=none.0.0" ||
    { echo "  [pid-wide] FAIL: v3 LOOP_NONE not rendered as none"; fail=1; }
echo "$out2" | grep -q "pid=1.3.4" ||
    { echo "  [pid-wide] FAIL: v2 pid not decoded"; fail=1; }
echo "$out2" | grep -q "pid=none.0.0" ||
    { echo "  [pid-wide] FAIL: v2 LOOP_NONE (0xFFFF) not mapped"; fail=1; }
if "$PY" "$VIEWER" "$TMPD/v9.xtcl" > "$TMPD/v9.out" 2>&1; then
	echo "  [pid-wide] FAIL: version 9 accepted"; fail=1
fi
[ "$fail" -eq 0 ] || { echo "$out3"; echo "$out2"; exit 1; }
echo "  [pid-wide] OK: xtc-tail.py reads v3 (u32/u32/u64, NONE 0xFFFFFFFF) and v2"

# ---- 2. live: gdb xtc-procs + xtc-tail-dump at slot 69,999 -------------
if ! command -v gdb >/dev/null 2>&1; then
	echo "  [pid-wide] SKIP (live part): gdb not on PATH"
	exit 0
fi
if [ -r /proc/sys/kernel/yama/ptrace_scope ] &&
    [ "$(cat /proc/sys/kernel/yama/ptrace_scope)" != "0" ]; then
	echo "  [pid-wide] SKIP (live part): ptrace_scope != 0"
	exit 0
fi
LIB="$XTC_BUILD_DIR/libxtc.a"
if [ ! -f "$LIB" ] || ! (cd "$XTC_BUILD_DIR" && ar p libxtc.a proc.o \
    2>/dev/null | grep -qa '\.debug_info'); then
	echo "  [pid-wide] SKIP (live part): libxtc.a missing or built without -g"
	exit 0
fi
# Two mappings per fiber stack: 70,000 procs need ~140,000 + headroom.
if [ -r /proc/sys/vm/max_map_count ] &&
    [ "$(cat /proc/sys/vm/max_map_count)" -lt 160000 ]; then
	echo "  [pid-wide] SKIP (live part): vm.max_map_count < 160000"
	exit 0
fi

cat > "$TMPD/prog.c" <<'PROGEOF'
#include <signal.h>
#include <stdio.h>
#include "xtc.h"
#include "xtc_async.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_res.h"
#include "xtc_tail.h"

#define N 70000

static void
parker(void *a)
{
	void *m = NULL;
	size_t sz;
	(void)a;
	(void)xtc_recv(&m, &sz, -1);
	xtc_free(m);
}

int
main(void)
{
	xtc_loop_t *lp = NULL;
	struct xtc_res *r;
	xtc_pid_t pid;
	int i;

	(void)xtc_set_stack_size(16 * 1024);
	if (xtc_loop_init(&lp) != XTC_OK)
		return 2;
	r = xtc_loop_res(lp);
	xtc_res_set_cap(r, XTC_RES_TASKS, 2 * N);
	xtc_res_set_cap(r, XTC_RES_FDS, 2 * N);
	xtc_res_set_cap(r, XTC_RES_INBOX_MSGS, 2 * N);
	xtc_res_set_cap(r, XTC_RES_MEM_BYTES, 8LL << 30);
	(void)xtc_tail_enable(XTC_TAIL_SCHED);
	for (i = 0; i < N; i++)
		if (xtc_proc_spawn(lp, parker, NULL, NULL, &pid) != XTC_OK) {
			fprintf(stderr, "spawn %d failed\n", i);
			return 3;
		}
	(void)raise(SIGSTOP);
	return 0;
}
PROGEOF

# The build's -fsanitize flags: a sanitizer build's libxtc.a needs its
# runtime at this link (the rest of LDFLAGS, e.g. a distribution's
# -specs=...-hardened-ld forcing PIE, does not fit this non-PIE probe).
# -fno-lto: a libxtc built with -ffat-lto-objects -flto (distribution
# hardened flags) would otherwise be re-optimized at this link, which
# keeps no debug location for its statics (__lt, __tail_seq: "optimized
# out"), and the helpers under test read them by name.
if ! ${CC:-cc} -O0 -g -w -fno-lto -I "$XTC_SRC_DIR/src/inc" -I "$XTC_BUILD_DIR" \
    -o "$TMPD/prog" "$TMPD/prog.c" "$LIB" \
    -pthread $(sed -n 's/^LDFLAGS[ \t]*=//p' "$XTC_BUILD_DIR/Makefile" | tr ' \t' '\n\n' | grep '^-fsanitize') \
    $(sed -n 's/^LIBS[ \t]*=[ \t]*//p' "$XTC_BUILD_DIR/Makefile") \
    > "$TMPD/cc.log" 2>&1; then
	echo "  [pid-wide] FAIL: probe program did not build"
	sed 's/^/    /' "$TMPD/cc.log" | head -20
	exit 1
fi

"$TMPD/prog" > "$TMPD/prog.out" 2>&1 &
PROG_PID=$!
i=0
st=
while [ "$i" -lt 300 ]; do
	st=$(awk '{print $3}' "/proc/$PROG_PID/stat" 2>/dev/null || echo "")
	[ "$st" = "T" ] && break
	[ -z "$st" ] && break
	i=$((i + 1))
	sleep 0.1
done
if [ "$st" != "T" ]; then
	echo "  [pid-wide] FAIL: probe never stopped: $(cat "$TMPD/prog.out")"
	exit 1
fi

gdb -q -p "$PROG_PID" -batch \
    -ex "source $GDBPY" \
    -ex "xtc-procs" \
    -ex "xtc-tail-dump $TMPD/live.xtcl" > "$TMPD/gdb.out" 2>&1 || true

grep -q " 0\.69999\.1 " "$TMPD/gdb.out" ||
    { echo "  [pid-wide] FAIL: xtc-procs did not list 0.69999.1"; fail=1; }
grep -q "^(70000 procs)" "$TMPD/gdb.out" ||
    { echo "  [pid-wide] FAIL: xtc-procs did not count 70000 procs"; fail=1; }
if [ -f "$TMPD/live.xtcl" ]; then
	ver=$(od -An -tu4 -j4 -N4 "$TMPD/live.xtcl" | tr -d ' ')
	[ "$ver" = "3" ] ||
	    { echo "  [pid-wide] FAIL: xtc-tail-dump wrote version $ver"; fail=1; }
	"$PY" "$VIEWER" "$TMPD/live.xtcl" --pid 0.69999.1 > "$TMPD/view.out" 2>&1
	grep -q "SPAWN.*pid=0.69999.1" "$TMPD/view.out" ||
	    { echo "  [pid-wide] FAIL: no SPAWN for 0.69999.1 in the dump"; fail=1; }
else
	echo "  [pid-wide] FAIL: xtc-tail-dump wrote nothing"; fail=1
fi
if [ "$fail" -ne 0 ]; then
	grep -v "^0x" "$TMPD/gdb.out" | tail -20 | sed 's/^/    /'
	exit 1
fi
echo "  [pid-wide] OK: gdb xtc-procs and xtc-tail-dump decode slot 69999 (v3)"

# ---- 3. the same stopped process under lldb (xtc-procs) ----------------
# --no-lldbinit: a user's ~/.lldbinit must not change what this checks.
if ! command -v lldb >/dev/null 2>&1; then
	echo "  [pid-wide] SKIP (lldb part): lldb not on PATH"
	exit 0
fi
LLDBPY="$XTC_SRC_DIR/tools/lldb/xtc_lldb.py"
timeout -k 10 600 lldb --no-lldbinit -b -p "$PROG_PID" \
    -o "command script import $LLDBPY" -o "xtc-procs" \
    > "$TMPD/lldb.out" 2>&1 || true
if grep -q "attach failed\|error: attach" "$TMPD/lldb.out"; then
	echo "  [pid-wide] SKIP (lldb part): lldb could not attach"
	exit 0
fi
grep -q " 0\.69999\.1 " "$TMPD/lldb.out" ||
    { echo "  [pid-wide] FAIL: lldb xtc-procs did not list 0.69999.1"; fail=1; }
grep -q "^(70000 procs)" "$TMPD/lldb.out" ||
    { echo "  [pid-wide] FAIL: lldb xtc-procs did not count 70000 procs"; fail=1; }
if [ "$fail" -ne 0 ]; then
	grep -v "^0x" "$TMPD/lldb.out" | tail -10 | sed 's/^/    /'
	exit 1
fi
echo "  [pid-wide] OK: lldb xtc-procs decodes slot 69999"
