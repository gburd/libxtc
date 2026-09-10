#!/bin/sh
# test/tools/test_xtc_tail_strands.sh
#
#	Gate the xtc-tail.py --strands classifier against traces whose
#	GROUND TRUTH is known by construction.
#
#	Why this exists.  --strands buckets a stranded fiber by how far its
#	wake got (no REAP / REAP,no WAKE / WAKE,no RUN), and the whole point
#	is that each bucket implicates a DIFFERENT subsystem.  The first
#	version got this inverted on real traces -- it reported 28 of 30
#	strands as "WAKE, no RUN" when all 30 were "no REAP" -- because a
#	task pointer is stable for the fiber's WHOLE LIFE and the WAKE/REAP
#	membership tests were not time-scoped to "after the final park".  Any
#	fiber with a healthy history was therefore guaranteed to match on its
#	own earlier, successful cycles.
#
#	That bug was SELF-CONCEALING: the more normal work a fiber did before
#	stranding, the more confidently it was misclassified.  The in-tree
#	C-level gate (/m12/tail/reap) missed it entirely because its fibers
#	have short histories.  So the discriminating input is specifically a
#	fiber with a LONG healthy history that then strands, and that is what
#	this builds.
#
#	Each synthesized fiber runs 150 healthy PARK/REAP/WAKE/RUN cycles and
#	then strands at a different step, so the expected answer is exactly
#	one fiber in each of the four buckets.  A classifier that ignores time
#	collapses three of them into "WAKE, no RUN" and fails here.

set -e

: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set}"

VIEWER="$XTC_SRC_DIR/tools/xtc-tail.py"
TMPD=$(mktemp -d)
TRACE="$TMPD/strands.xtcl"

cleanup() {
	find "$TMPD" -mindepth 1 -delete 2>/dev/null || true
	rmdir "$TMPD" 2>/dev/null || true
}
trap cleanup 0 1 2 15

if [ ! -f "$VIEWER" ]; then
	echo "[tail-strands] SKIP: $VIEWER not found"
	exit 0
fi

PY=$(command -v python3 2>/dev/null || true)
if [ -z "$PY" ]; then
	echo "[tail-strands] SKIP: python3 not available"
	exit 0
fi

# Synthesize the trace.  Format matches tools/xtc-tail.py's parse(): a
# 24-byte little-endian header (magic, version, flags, count, base_ts)
# followed by count records of (kind u8, source u8, then LEB128 dts,
# loop, local, gen, detail).
"$PY" - "$TRACE" <<'PYEOF'
import struct, sys

MAGIC = 0x5854434C
FLAG_LE = 1
WAKE, RUN, PARK, LOOP_POLL, PARK_TASK, REAP = 2, 3, 4, 8, 9, 10
SUBMIT = 11


def leb(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            break
    return bytes(out)


evs = []
ts = [1000]


def add(kind, loop, local, gen, detail):
    ts[0] += 100
    evs.append((kind, 0, ts[0], loop, local, gen, detail))


# Four fibers.  Each gets a long healthy history -- which is the input that
# exposes a time-blind join -- then strands at a different step.
cases = [(0x9000, "nosubmit"), (0xA000, "submit"), (0xB000, "reap"),
         (0xC000, "wake"), (0xD000, "run")]
for i, (task, reached) in enumerate(cases):
    loc = i + 1
    for _ in range(150):
        # ORDER MATTERS AND MUST MATCH THE LIBRARY: the SQE is queued FIRST,
        # then the fiber parks waiting for its completion.  So SUBMIT is
        # strictly EARLIER than the park it belongs to, while REAP/WAKE/RUN
        # are strictly later.  The first version of this gate emitted SUBMIT
        # after the park and therefore validated the classifier's own wrong
        # assumption instead of testing it -- the bug shipped and a consumer
        # caught it.  A synthesized trace is only a control if its shape
        # matches what the library really produces.
        add(SUBMIT, 3, 0, 0, task)
        add(PARK_TASK, 24, loc, 1, task)
        add(PARK, 24, loc, 1, 837)
        add(REAP, 3, 0, 0, task)
        add(WAKE, 3, 0, 0, task)
        add(RUN, 24, loc, 1, 5000)
    # The final park.  "submit" and deeper get a SUBMIT BEFORE the park;
    # "nosubmit" gets none, which is the only honest way to represent a
    # request that was never queued.
    if reached in ("submit", "reap", "wake", "run"):
        add(SUBMIT, 3, 0, 0, task)
    add(PARK_TASK, 24, loc, 1, task)
    add(PARK, 24, loc, 1, 822)
    if reached in ("reap", "wake", "run"):
        add(REAP, 3, 0, 0, task)
    if reached in ("wake", "run"):
        add(WAKE, 3, 0, 0, task)
    if reached == "run":
        add(RUN, 24, loc, 1, 7000)
for _ in range(3):
    add(LOOP_POLL, 3, 0, 0, 0)

body = bytearray()
prev = 1000
for (k, s, t, lp, lo, g, d) in evs:
    body.append(k)
    body.append(s)
    body += leb(t - prev)
    prev = t
    body += leb(lp) + leb(lo) + leb(g) + leb(d)

hdr = struct.pack("<IIII", MAGIC, 1, FLAG_LE, len(evs))
hdr += struct.pack("<Q", 1000)
open(sys.argv[1], "wb").write(hdr + bytes(body))
PYEOF

OUT="$TMPD/out.txt"
"$PY" "$VIEWER" "$TRACE" --strands > "$OUT" 2>&1 || {
	echo "[tail-strands] FAIL: --strands exited nonzero"
	cat "$OUT"
	exit 1
}

fail=0
for bucket in "never submitted" "no REAP" "REAP, no WAKE" "WAKE, no RUN" \
    "resumed"; do
	# the summary lines are "  <bucket>   <count>"
	got=$(grep "^  $bucket " "$OUT" | awk '{print $NF}')
	if [ "$got" != "1" ]; then
		echo "[tail-strands] FAIL: bucket '$bucket' = '$got', expected 1"
		fail=1
	fi
done

if [ "$fail" -ne 0 ]; then
	echo "--- --strands output ---"
	cat "$OUT"
	echo "Each synthesized fiber strands at a DIFFERENT step, so every"
	echo "bucket must hold exactly one.  Several buckets collapsing into"
	echo "one (classically all into 'WAKE, no RUN') means the classifier"
	echo "is matching a task pointer from the fiber's healthy history"
	echo "instead of scoping the match to after its final park."
	exit 1
fi

echo "[tail-strands] OK: --strands separates all five wake-progress buckets"
echo "          for fibers with long healthy histories (the time-scoping"
echo "          a stable task pointer requires)"
exit 0
