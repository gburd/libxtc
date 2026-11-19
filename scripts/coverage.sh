#!/bin/sh
# scripts/coverage.sh -- MERGED coverage across both build configurations.
#
#	`make check` and `make check-dst` are SEPARATE configurations: the
#	DST tier builds its own --with-io-backend=sim library.  So a gcov
#	run over `make check` alone structurally UNDERCOUNTS -- everything
#	only the sim backend reaches (src/evt/sim.c, and the sim-only
#	branches of evt/exec.c and evt/loop.c) reads as uncovered.  That is
#	how evt/sim.c came to look ~16% covered while being exercised by 65
#	DST tests.
#
#	This script measures BOTH tiers with instrumentation and merges
#	them.  Next to the merged totals it prints the coverage of the
#	DST-reachable public surface that AGENTS.md states the claim against,
#	as scripts/dst_surface.py defines it (the rule is printed with it),
#	covered by any tier and by the DST tier alone.
#
#	Usage:
#	    sh scripts/coverage.sh [OUTDIR]      # default: ./coverage-out
#	Requires gcovr and gcc (the nix dev shell provides both).
#	Prints the merged line/branch/function percentages and writes a
#	browsable HTML report.
#
#	A failing test does not stop the measurement: every C, PBT and shell
#	test and every DST test runs (make -k CHECK_KEEP_GOING=1 check), the
#	failures are listed next to the figure, and the script exits 1 after
#	printing it.  Under -O0 --coverage some timing tests fail that pass
#	in an optimized build; a figure from such a run says so.

set -eu

SRC_DIR=${XTC_SRC_DIR:-$(cd "$(dirname "$0")/.." && pwd)}
OUT=${1:-$PWD/coverage-out}
COV_CC=${CC:-gcc}

if ! command -v gcovr >/dev/null 2>&1; then
	echo "  [coverage] SKIP: gcovr not on PATH (nix develop provides it)"
	exit 0
fi

mkdir -p "$OUT"
BUILD="$OUT/build-check"
mkdir -p "$BUILD"
FAILS="$OUT/failures.txt"
: >"$FAILS"

# ---- 1. the ordinary configuration: make check -------------------------
# A build failure leaves nothing to measure, so it stops the script.  A test
# failure does not: each tier lists its failing tests in check-fails-*.txt,
# and make -k runs the other tiers (and the examples) past a failed one.
echo "  [coverage] 1/3 building + running make check (instrumented)"
(
	cd "$BUILD" &&
	CFLAGS="--coverage -O0 -g" LDFLAGS="--coverage" \
	    "$SRC_DIR/dist/configure" --with-tls=auto >configure.log 2>&1 &&
	make -j"$(nproc 2>/dev/null || echo 2)" >build.log 2>&1
) || {
	echo "  [coverage] FAIL: instrumented build failed"
	echo "             see $BUILD/configure.log and $BUILD/build.log"
	exit 1
}
# A tier whose recipe never ran (one of its test programs failed to
# build) writes no check-fails file; say so rather than measure without it.
for tier in c pbt sh; do
	rm -f "$BUILD/check-fails-$tier.txt"
done
check_rc=0
(cd "$BUILD" && "${MAKE:-make}" -k CHECK_KEEP_GOING=1 check) \
    >"$BUILD/check.log" 2>&1 || check_rc=$?
for tier in c pbt sh; do
	if [ ! -f "$BUILD/check-fails-$tier.txt" ]; then
		echo "[$tier] the tier did not run (see $BUILD/check.log)" >>"$FAILS"
	elif [ -s "$BUILD/check-fails-$tier.txt" ]; then
		sed "s/^/[$tier] /" "$BUILD/check-fails-$tier.txt" >>"$FAILS"
	fi
done
# Any other target that failed (the examples, or a test that did not
# build); the three tiers' failures are already listed by test.
sed -n 's/^.*\*\*\* \[[^]]*: *\([^]]*\)\] Error.*$/\1/p' \
    "$BUILD/check.log" | grep -vxE 'tests-(c|pbt|sh)' |
    sed 's/^/[make] target failed: /' >>"$FAILS" || true
if [ "$check_rc" -ne 0 ] && [ ! -s "$FAILS" ]; then
	echo "[make] make check exited $check_rc (see $BUILD/check.log)" >>"$FAILS"
fi
gcovr -r "$SRC_DIR/src" --object-directory "$BUILD" \
    --json -o "$OUT/tf-check.json" >/dev/null 2>&1 || true

# ---- 2. the sim configuration: the DST tier ----------------------------
# run_sim_tests.sh configures its own --with-io-backend=sim build; it
# honors CFLAGS/LDFLAGS from the environment, and XTC_SIM_KEEP_BUILD keeps
# the build dir (with its .gcda) instead of a throwaway mktemp dir.  CC
# carries --coverage too because the script links the test binaries itself.
# run_sim_tests.sh runs every DST test past a failure and prints
# "[sim] FAIL: <test>" for each.
#
# The sim library is also compiled with gcc's call-graph and CFG dumps, from
# which scripts/dst_surface.py computes the DST-reachable surface (its
# docstring holds the rules and why).  Its self-test runs first: if gcc's
# dump format moved, the surface figure is not printed and the run fails,
# rather than quote a figure from a misread graph.
SURF_FLAGS=
if python3 "$SRC_DIR/scripts/dst_surface.py" --self-test --cc "$COV_CC" \
    >"$OUT/surface-selftest.log" 2>&1; then
	SURF_FLAGS="-fdump-ipa-cgraph -fdump-tree-cfg-blocks-lineno"
else
	echo "[surface] scripts/dst_surface.py --self-test failed" \
	    "(see $OUT/surface-selftest.log)" >>"$FAILS"
fi
echo "  [coverage] 2/3 building + running the DST tier (instrumented)"
dst_rc=0
CFLAGS="--coverage -O0 -g $SURF_FLAGS" LDFLAGS="--coverage" \
CC="$COV_CC --coverage" \
XTC_SIM_KEEP_BUILD="$OUT/simbuild" \
XTC_SRC_DIR="$SRC_DIR" \
    sh "$SRC_DIR/test/sim/run_sim_tests.sh" >"$OUT/dst.log" 2>&1 || dst_rc=$?
n_before=$(wc -l <"$FAILS")
sed -n 's/^ *\[sim\] FAIL: /[dst] /p' "$OUT/dst.log" >>"$FAILS"
if [ "$dst_rc" -ne 0 ] && [ "$(wc -l <"$FAILS")" -eq "$n_before" ]; then
	echo "[dst] run_sim_tests.sh exited $dst_rc (see $OUT/dst.log)" >>"$FAILS"
fi
gcovr -r "$SRC_DIR/src" --object-directory "$OUT/simbuild/build" \
    --json -o "$OUT/tf-dst.json" >/dev/null 2>&1 || true
SURFACE=
rm -f "$OUT/surface.json"
if [ -n "$SURF_FLAGS" ]; then
	if python3 "$SRC_DIR/scripts/dst_surface.py" --map "$SRC_DIR/dist/libxtc.map" \
	    --out "$OUT/surface.json" "$OUT/simbuild/build" >"$OUT/surface.log" 2>&1; then
		SURFACE="--surface $OUT/surface.json"
	else
		echo "[surface] scripts/dst_surface.py failed (see $OUT/surface.log)" >>"$FAILS"
	fi
fi

# ---- 3. merge ---------------------------------------------------------
echo "  [coverage] 3/3 merging"
gcovr --add-tracefile "$OUT/tf-check.json" \
      --add-tracefile "$OUT/tf-dst.json" \
      --json-summary-pretty -o "$OUT/merged-summary.json" >/dev/null 2>&1 ||
    true
# A browsable per-file HTML view is a nice-to-have, and it needs the SOURCE
# files at the paths recorded in each tracefile.  Merging two build dirs can
# leave some of those unresolvable, and gcovr then exits non-zero ("N source
# file(s) not found") -- which must not fail the whole measurement.  The
# authoritative numbers come from the summary JSON above.
if ! gcovr --add-tracefile "$OUT/tf-check.json" \
      --add-tracefile "$OUT/tf-dst.json" \
      --html-details "$OUT/merged.html" >"$OUT/html.log" 2>&1; then
	echo "  [coverage] note: per-file HTML view incomplete (see $OUT/html.log);"
	echo "             the merged totals below are unaffected."
fi

# Report.  gcovr's own summary JSON is the source of truth; parse it with a
# tiny inline program (kept out of a heredoc so `set -e` cannot trip on the
# here-document's exit status under some shells).
# Authoritative numbers come from gcov's own JSON, aggregated by
# scripts/coverage_summary.py.  gcovr 8.2 SILENTLY DROPS some translation
# units -- it reported 0 lines for src/os/os_time.c, os_env.c, os_rand.c,
# os_thread.c, os_alloc.c, os_errno.c and os_mutex.c while `gcov` itself
# reports real coverage for each (os_time.c: 75% of 36 lines) -- which
# understated the whole os layer.  The gcovr summary/HTML above is kept as
# a convenient browsable view, but do not quote it.
# shellcheck disable=SC2086  # SURFACE is empty or one option and its file
python3 "$SRC_DIR/scripts/coverage_summary.py" --failures "$FAILS" $SURFACE \
    "$BUILD" "$OUT/simbuild/build"

echo "  [coverage] logs: $BUILD/check.log, $OUT/dst.log"
echo "  [coverage] detail: $OUT/merged-summary.json (+ merged.html)"
if [ -s "$FAILS" ]; then
	echo "  [coverage] FAIL: $(wc -l <"$FAILS" | tr -d ' ') failing test(s); see above"
	exit 1
fi
