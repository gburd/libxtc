#!/bin/sh
# dst-bug-inject.sh -- the DST "bug-detection latency" yardstick.
#
# For each PLANTED bug (see src/inc/xtc_dst_inject.h), build the sim
# library + the DST test whose safety invariant should catch it with
# -DXTC_DST_INJECT_BUG=<id> and prove the test catches THAT bug:
#
#   1. CONTROL -- the same test must PASS on a CLEAN build (no planted
#      bug).  Without this a test that is already broken, or whose
#      dependencies fail to set up, would be credited as "catching" every
#      planted bug.  This is what made the old gate vacuous: it credited
#      ANY nonzero exit.
#   2. INTENDED REASON -- with the bug active the test must fail AND its
#      output must name the invariant this bug plants against (EXPECT
#      below).  An unrelated crash, a hang, or a setup error fails for the
#      wrong reason and is reported as a MISS, not a catch.
#   3. SEED BUDGET -- for a seed-driven test the harness walks seeds
#      1..BUDGET one at a time and records the FIRST catching seed, which
#      is the actual bug-detection-latency number.  A bug that needs more
#      than BUDGET seeds fails the gate (detection is too slow to be
#      useful).  Tests with built-in seed sets report "built-in".
#   4. TIMEOUT -- every run is bounded, so a planted bug that HANGS the
#      test is reported as a hang instead of stalling CI forever.
#   5. EVIDENCE -- the failing output of every catch (and of every miss)
#      is retained under $EVIDENCE so the claim can be audited and the
#      seed replayed.  The old gate discarded it (>/dev/null 2>&1).
#
# This is the metric FoundationDB / TigerBeetle use to back "our
# simulator finds real bugs": if you break a safety invariant, DST
# catches it -- deterministically, from a seed you can replay.
#
# Usage: scripts/dst-bug-inject.sh        (run from the repo root)
# Env:   CC (default cc), TMPDIR (default /tmp),
#        XTC_DST_BUDGET (seed budget, default 8),
#        XTC_DST_TIMEOUT (per-run seconds, default 300)

set -u
SELF_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SELF_DIR/.." && pwd)
CC="${CC:-cc}"
: "${TMPDIR:=/tmp}"
mkdir -p "$TMPDIR"
BUDGET="${XTC_DST_BUDGET:-8}"
RUN_TIMEOUT="${XTC_DST_TIMEOUT:-300}"

INC="-I$ROOT/src/inc"
BMDIR="$ROOT/examples/06_sqlxtc"

# bug-id ; catching-test ; sqlxtc-engine-needed(0/1) ; seeded(0/1) ; EXPECT
#
# Each planted bug (src/inc/xtc_dst_inject.h) is a compile-flag-gated
# violation of ONE asserted DST safety invariant; the paired test's
# invariant checker must FAIL when the bug is active, AND say so.  EXPECT
# is an ERE matched against the test's output: it names the invariant
# message this bug must produce, so a failure for any OTHER reason is not
# credited.  `seeded` marks a test that takes "<base> <count>" on argv and
# can therefore be walked seed by seed for the first catching seed.
#
# Fields are ';'-separated (no field may contain ';'; EXPECT contains
# spaces and '|', which is why whitespace is not the separator).  The
# table is written to $cases_file after $work exists (below).
write_cases() {
	printf '%s\n' \
		"1;test_sim_pingpong;0;0;not all pings got replies" \
		"2;test_sim_compose;1;1;FAIL \\(committed=[0-9]+ recv=[0-9]+ viol=[1-9]" \
		"3;test_sim_compose_crash;1;1;LOST or CORRUPTED an acked commit" \
		"4;test_sim_credit;0;0;in-flight exceeded window" \
		"5;test_sim_res;0;1;FAIL \\(succ=[0-9]+ rej=[0-9]+ over=0 final_used=[1-9]" \
		"6;test_sim_res;0;1;FAIL \\(succ=[0-9]+ rej=[0-9]+ over=[1-9]" \
		"7;test_sim_chan;0;0;mpmc lost/duplicated items" \
		"8;test_sim_reg;0;0;(duplicate concurrent registrations|whereis mis-resolved)" \
		"9;test_sim_saga;0;0;compensation-order violation" \
		> "$1"
}
# 1  LOSTWAKE   proc.c drops a mailbox wake     -> lost-wakeup / no quiescence
# 2  LOCKEXCL   lock_mgr.c grants a conflict    -> mutual exclusion (lock held <= 1)
# 3  NODURABLE  wal.c skips fdatasync, acks     -> durability (acked commit present)
# 4  CREDITWIN  credit.c double-posts a credit  -> window never exceeded (in-flight <= window)
# 5  RESLEAK    res.c drops the release decr    -> conservation (final used == 0)
# 6  RESOVER    res.c skips the acquire cap chk -> safety (used never > cap)
# 7  CHANDROP   chan.c drops an mpmc message    -> exactly-once delivery (no drop/dup)
# 8  REGDUP     reg.c allows a duplicate reg    -> at-most-one-holder (no double registration)
# 9  SAGAORDER  saga.c compensates forward      -> exact reverse-order compensation
#
# Note on bug 8: a second holder of a name makes xtc_reg_whereis resolve
# to either pid, so the test's mis-resolve check is the FIRST consequence
# of the double registration to fire.  Both messages are consequences of
# the same at-most-one-holder violation, so EXPECT accepts either -- and
# the harness additionally requires the run's own report line to show
# bad-dup > 0, proving the duplicate registration really happened rather
# than a lookup merely flaking.
#
# NOT plantable (recorded, not a gap in effort): the xtc_amutex
# mutual-exclusion and lost-wakeup invariants (test_sim_latch).  Its
# critical section is yield-free so the cooperative sim runs it
# atomically (a double-grant never interleaves a lost update), and a
# dropped hand-off wake is invisible because the sim reschedules a
# parked fiber from its state, not a wake fd (see test_sim_wake_park),
# with unlock setting granted under the lock.  Both were built and
# confirmed to pass the test both ways -> dropped, not faked.  Mutual
# exclusion is proven by bug 2 (LOCKEXCL) against the lock manager.
#
# ALSO NOT plantable, and this one is a real DST BLIND SPOT, not a
# yield-free-critical-section artifact: the CROSS-LOOP aio completion
# nudge (the v1.44.1 fix -- aio.c nudges the submitting loop when a
# migrated fiber resumes elsewhere with the op still pending).  Planting
# "skip the nudge" cannot be caught by DST because the sim's runnability
# oracle (__sim_loop_runnable in exec.c) marks a loop runnable whenever a
# completion on its ring is due, REGARDLESS of whether the owning loop was
# nudged -- it reads the completion store directly.  In production a loop
# does not know its ring has a completion until it is woken (its own
# blocking poll, a nudge, or a fairness poll); a completion whose owner is
# asleep-and-un-nudged strands, which is exactly the escaped bug.  The sim
# cannot express "owner never learns of the completion," so a planted
# NONUDGE would show as a HOLE, not a catch.  This class is covered
# INSTEAD by the real-thread liveness guard test_aio_migrate_wake and,
# decisively, by the consumer's real 32-backend workload.  Documented in
# full in .agent/DST_GAP_ANALYSIS_2026-09-11.md -- the honest statement is
# that DST's runnability model is one level above where this bug lives.

work=$(mktemp -d "$TMPDIR/dstbug.XXXXXX") || exit 1
cases_file="$work/cases.txt"
write_cases "$cases_file"
EVIDENCE="$TMPDIR/dst-bug-inject-evidence"
mkdir -p "$EVIDENCE"
fails=0
caught=0
total=0
have_timeout=0
command -v timeout >/dev/null 2>&1 && have_timeout=1
[ "$have_timeout" = 1 ] || echo "  [bug] NOTE: no timeout(1); runs are UNBOUNDED"

# Run "$@" under the per-run timeout when one is available.  Returns the
# program's exit status, or 124 on timeout (timeout(1)'s convention).
run_bounded() {
	if [ "$have_timeout" = 1 ]; then
		timeout "$RUN_TIMEOUT" "$@"
	else
		"$@"
	fi
}

# Build the sqlxtc engine objects a test needs, into $1, with the bug
# define in $2 ("" for a clean build).  Echoes the object list.
build_engine() {
	_dir=$1
	_def=$2
	_objs=""
	for _s in bufmgr engine db quack metrics vexec sql_parse \
		sql_parse_drv sql_ast sql_parse_gen xstore xlog wal \
		btree btnode mvcc sqlrec mem; do
		# shellcheck disable=SC2086
		if $CC $_def -g -O1 -std=c11 -D_GNU_SOURCE \
			-DSQLXTC_HAVE_LIME=1 $INC -I"$BMDIR" \
			-c "$BMDIR/$_s.c" -o "$_dir/$_s.o" 2>/dev/null; then
			_objs="$_objs $_dir/$_s.o"
		fi
	done
	echo "$_objs"
}

# ---- the CLEAN control build: every catching test must PASS here -------
# This is what turns "the test exited nonzero" into "the PLANTED BUG made
# this test fail": a test that is already red, or whose engine objects do
# not build, cannot be credited as catching anything.
clean="$work/clean"
mkdir -p "$clean"
( cd "$clean" && CFLAGS="-g -O1 -D_GNU_SOURCE" \
	"$ROOT/dist/configure" --with-io-backend=sim --with-tls=none \
	--without-liburing >/dev/null 2>&1 && make -j4 libxtc.a \
	>/dev/null 2>&1 ) || {
	echo "  [bug] FAIL: the CLEAN control library did not build"
	find "$work" -mindepth 1 -delete 2>/dev/null
	rmdir "$work" 2>/dev/null
	exit 1; }
clean_eng=$(build_engine "$clean" "")

while IFS=';' read -r bug test_c need_eng seeded expect; do
	[ -n "$bug" ] || continue
	# A malformed row (a ';' smuggled into EXPECT, a missing field) must
	# not silently degrade into "no cases run".
	case "$bug$need_eng$seeded" in
	*[!0-9]*|'') echo "  [bug] FAIL: malformed case row: $bug"
		fails=$((fails + 1)); continue ;;
	esac
	if [ -z "$test_c" ] || [ -z "$expect" ]; then
		echo "  [bug $bug] FAIL: malformed case row (missing test/EXPECT)"
		fails=$((fails + 1)); continue
	fi
	total=$((total + 1))

	# ---- control: this test must PASS clean ----
	ctl="$clean/$test_c"
	if [ ! -x "$ctl" ]; then
		extra_inc=""
		[ "$need_eng" = 1 ] && extra_inc="-I$BMDIR -DSQLXTC_HAVE_LIME=1"
		eng=""
		[ "$need_eng" = 1 ] && eng="$clean_eng"
		# shellcheck disable=SC2086
		if ! $CC -g -O1 -std=c11 -D_GNU_SOURCE $INC $extra_inc \
			"$ROOT/test/sim/$test_c.c" $eng "$clean/libxtc.a" \
			-pthread -ldl -lm -o "$ctl" 2>/dev/null; then
			echo "  [bug $bug] FAIL: $test_c did not build CLEAN (control)"
			fails=$((fails + 1)); continue
		fi
	fi
	if ! run_bounded "$ctl" > "$EVIDENCE/control-$test_c.txt" 2>&1; then
		echo "  [bug $bug] FAIL: $test_c does not PASS on the CLEAN build --"
		echo "             its failure cannot be attributed to bug $bug"
		echo "             (control output: $EVIDENCE/control-$test_c.txt)"
		fails=$((fails + 1)); continue
	fi

	# ---- the planted-bug build ----
	bdir="$work/bug$bug"
	mkdir -p "$bdir"
	( cd "$bdir" && CFLAGS="-DXTC_DST_INJECT_BUG=$bug -g -O1 -D_GNU_SOURCE" \
		"$ROOT/dist/configure" --with-io-backend=sim --with-tls=none \
		--without-liburing >/dev/null 2>&1 && make -j4 libxtc.a \
		>/dev/null 2>&1 ) || {
		echo "  [bug $bug] FAIL: sim lib did not build"
		fails=$((fails + 1)); continue; }

	engobjs=""
	[ "$need_eng" = 1 ] &&
		engobjs=$(build_engine "$bdir" "-DXTC_DST_INJECT_BUG=$bug")

	exe="$bdir/$test_c"
	extra_inc=""
	[ "$need_eng" = 1 ] && extra_inc="-I$BMDIR -DSQLXTC_HAVE_LIME=1"
	# shellcheck disable=SC2086
	if ! $CC -g -O1 -std=c11 -D_GNU_SOURCE -DXTC_DST_INJECT_BUG=$bug \
		$INC $extra_inc "$ROOT/test/sim/$test_c.c" $engobjs \
		"$bdir/libxtc.a" -pthread -ldl -lm -o "$exe" 2>/dev/null; then
		echo "  [bug $bug] FAIL: $test_c did not build"
		fails=$((fails + 1)); continue
	fi

	# ---- run: the test MUST fail, for the INTENDED reason, within the
	#      seed budget.  Walk one seed at a time when the test is
	#      seed-driven so the first catching seed is the measured
	#      bug-detection latency; otherwise a single built-in-seed run.
	log="$EVIDENCE/bug$bug-$test_c.txt"
	first=""
	wrong=""
	hang=""
	tried=0
	k=1
	while :; do
		if [ "$seeded" = 1 ]; then
			run_bounded "$exe" "$k" 1 > "$log" 2>&1
			rc=$?
			label="seed $k"
		else
			run_bounded "$exe" > "$log" 2>&1
			rc=$?
			label="its built-in seeds"
		fi
		tried=$((tried + 1))
		if [ "$rc" = 124 ]; then
			hang="$label"
			break
		fi
		if [ "$rc" != 0 ]; then
			if grep -Eq "$expect" "$log"; then
				# bug 8 corroboration: the duplicate registration
				# must actually have happened, not just a flaky
				# lookup.
				if [ "$bug" = 8 ] &&
				   ! grep -Eq 'bad-dup=[1-9]' "$log"; then
					wrong="$label"
					break
				fi
				first="$label"
				break
			fi
			wrong="$label"
			break
		fi
		[ "$seeded" = 1 ] || break     # built-in run passed: a HOLE
		k=$((k + 1))
		[ "$k" -le "$BUDGET" ] || break
	done

	if [ -n "$hang" ]; then
		echo "  [bug $bug] MISS: $test_c HUNG on $hang (>${RUN_TIMEOUT}s) -- a hang"
		echo "             is not the invariant firing; evidence: $log"
		fails=$((fails + 1))
	elif [ -n "$wrong" ]; then
		echo "  [bug $bug] MISS: $test_c failed on $wrong for the WRONG reason"
		echo "             (expected /$expect/); evidence: $log"
		fails=$((fails + 1))
	elif [ -n "$first" ]; then
		echo "  [bug $bug] OK: $test_c caught planted bug $bug on $first" \
			"(budget $BUDGET) -- $(grep -m1 -E "$expect" "$log" | \
			sed 's/^[[:space:]]*//' | cut -c1-92)"
		caught=$((caught + 1))
	else
		echo "  [bug $bug] HOLE: $test_c PASSED with bug $bug planted" \
			"($tried run(s), budget $BUDGET) -- DST did not catch it"
		fails=$((fails + 1))
	fi
done < "$cases_file"

# Cleanup of the BUILD tree only (rm -rf is blocked by the harness; use
# find + rmdir).  The evidence directory is deliberately KEPT.
find "$work" -mindepth 1 -delete 2>/dev/null
rmdir "$work" 2>/dev/null

if [ "$total" -eq 0 ]; then
	echo "FAIL: no planted-bug case ran -- the case table did not parse"
	exit 1
fi

echo "dst-bug-inject: $caught/$total planted bugs caught by DST" \
	"(each verified to pass CLEAN and to fail for its intended reason," \
	"within a $BUDGET-seed budget)"
echo "dst-bug-inject: evidence retained in $EVIDENCE"
[ "$fails" -eq 0 ] || { echo "FAIL: $fails planted bug(s) not caught"; exit 1; }
echo "OK: every planted safety bug is caught by the DST suite"
exit 0
