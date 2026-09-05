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
#	This script measures BOTH tiers with instrumentation and merges the
#	gcovr tracefiles.  The merged figure is the only honest one to quote
#	for "the DST-reachable public surface" that AGENTS.md asks for.
#
#	Usage:
#	    sh scripts/coverage.sh [OUTDIR]      # default: ./coverage-out
#	Requires gcovr and gcc (the nix dev shell provides both).
#	Prints the merged line/branch/function percentages and writes a
#	browsable HTML report.

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

# ---- 1. the ordinary configuration: make check -------------------------
echo "  [coverage] 1/3 building + running make check (instrumented)"
(
	cd "$BUILD"
	CFLAGS="--coverage -O0 -g" LDFLAGS="--coverage" \
	    "$SRC_DIR/dist/configure" --with-tls=auto >configure.log 2>&1
	make -j"$(nproc 2>/dev/null || echo 2)" >build.log 2>&1
	make check >check.log 2>&1
) || {
	echo "  [coverage] FAIL: instrumented make check failed"
	echo "             see $BUILD/check.log"
	exit 1
}
gcovr -r "$SRC_DIR/src" --object-directory "$BUILD" \
    --json -o "$OUT/tf-check.json" >/dev/null 2>&1

# ---- 2. the sim configuration: the DST tier ----------------------------
# run_sim_tests.sh configures its own --with-io-backend=sim build; it
# honors CFLAGS/LDFLAGS from the environment, and XTC_SIM_KEEP_BUILD keeps
# the build dir (with its .gcda) instead of a throwaway mktemp dir.  CC
# carries --coverage too because the script links the test binaries itself.
echo "  [coverage] 2/3 building + running the DST tier (instrumented)"
CFLAGS="--coverage -O0 -g" LDFLAGS="--coverage" \
CC="$COV_CC --coverage" \
XTC_SIM_KEEP_BUILD="$OUT/simbuild" \
XTC_SRC_DIR="$SRC_DIR" \
    sh "$SRC_DIR/test/sim/run_sim_tests.sh" >"$OUT/dst.log" 2>&1 || {
	echo "  [coverage] FAIL: instrumented DST tier failed"
	echo "             see $OUT/dst.log"
	exit 1
}
gcovr -r "$SRC_DIR/src" --object-directory "$OUT/simbuild/build" \
    --json -o "$OUT/tf-dst.json" >/dev/null 2>&1

# ---- 3. merge ---------------------------------------------------------
echo "  [coverage] 3/3 merging"
gcovr --add-tracefile "$OUT/tf-check.json" \
      --add-tracefile "$OUT/tf-dst.json" \
      --json-summary-pretty -o "$OUT/merged-summary.json" >/dev/null 2>&1
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
python3 "$SRC_DIR/scripts/coverage_summary.py" \
    "$BUILD" "$OUT/simbuild/build"

echo "  [coverage] detail: $OUT/merged-summary.json (+ merged.html)"
