#!/bin/sh
# dst-corpus.sh -- the DST failing-seed regression gate.
#
# Reads test/sim/corpus/*.txt (pinned <test, seed> pairs, see
# test/sim/corpus/README.md), builds the sim-backend library once,
# compiles each distinct pinned test once, and re-runs every pinned
# (test, seed) on the CURRENT clean build -- asserting each PASSES.
#
# This is the "a fixed bug cannot silently regress" gate (gap #2 of
# .agent/DST_MATURITY_2026-07.md): a seed that once reproduced a bug (or
# that a planted bug in scripts/dst-bug-inject.sh proves is caught) is
# pinned here so a future change that reintroduces the bug -- or weakens
# the invariant that catches it -- turns this red.
#
# The FAIL direction (bug active -> the test traps) is dst-bug-inject.sh;
# this is the PASS direction (bug fixed -> the seed stays fixed).  CI
# runs both side by side in the sim-dst job.
#
# Usage: scripts/dst-corpus.sh    (run from the repo root)
# Env:   CC (default cc), TMPDIR (default /tmp)

set -u
SELF_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SELF_DIR/.." && pwd)
CC="${CC:-cc}"
: "${TMPDIR:=/tmp}"
mkdir -p "$TMPDIR"

CORPUS_DIR="$ROOT/test/sim/corpus"
INC="-I$ROOT/src/inc"

if [ ! -x "$ROOT/dist/configure" ]; then
	echo "  [corpus] SKIP: dist/configure not generated"
	exit 0
fi
if [ ! -d "$CORPUS_DIR" ]; then
	echo "  [corpus] SKIP: no corpus directory"
	exit 0
fi

work=$(mktemp -d "$TMPDIR/dstcorpus.XXXXXX") || exit 1
# Cleanup (rm -rf is blocked by the harness; use find + rmdir).
# shellcheck disable=SC2329  # invoked indirectly via trap
cleanup() { find "$work" -mindepth 1 -delete 2>/dev/null; rmdir "$work" 2>/dev/null; }
trap cleanup EXIT INT TERM

# Build the clean sim-backend library once (no planted bug, no TLS, no
# liburing -- the sim backend has no platform deps).
build="$work/build"
mkdir -p "$build"
(
	cd "$build" && CFLAGS="-g -O1 -D_GNU_SOURCE" \
		"$ROOT/dist/configure" --with-io-backend=sim --with-tls=none \
		--without-liburing >/dev/null 2>&1 &&
		make -j"$(nproc 2>/dev/null || echo 2)" libxtc.a >/dev/null 2>&1
) || { echo "  [corpus] FAIL: sim library did not build"; exit 1; }
lib="$build/libxtc.a"

case "$(uname -s)" in
Darwin) libs="-pthread -lm" ;;
*)      libs="-pthread -ldl -lm" ;;
esac

# Compile a distinct pinned test once, caching the executable in $work.
# Prints the exe path on stdout, or nothing on a build failure.
compile_test() {
	_t=$1
	_exe="$work/$_t"
	[ -x "$_exe" ] && { echo "$_exe"; return 0; }
	# inc/libs intentionally word-split (each holds several flags).
	# shellcheck disable=SC2086
	if $CC -g -O1 -std=c11 -D_GNU_SOURCE $INC \
		"$ROOT/test/sim/$_t.c" "$lib" $libs -o "$_exe" 2>/dev/null; then
		echo "$_exe"
	fi
}

fails=0
total=0
pass=0

for f in "$CORPUS_DIR"/*.txt; do
	[ -f "$f" ] || continue
	# Read four whitespace-separated fields; the rest is the description.
	while read -r test seed count desc; do
		# Skip blanks and comments.
		case "$test" in ''|\#*) continue ;; esac
		total=$((total + 1))

		exe=$(compile_test "$test")
		if [ -z "$exe" ]; then
			echo "  [corpus] FAIL: $test did not build"
			fails=$((fails + 1))
			continue
		fi

		# A seed of "-" -> run with no argv (built-in fixed seeds);
		# otherwise pass "<seed> <count>" to the argv-driven test.
		if [ "$seed" = "-" ]; then
			run_desc="$test (built-in seeds)"
			ok=0
			"$exe" >/dev/null 2>&1 && ok=1
		else
			run_desc="$test seed=$seed count=$count"
			ok=0
			"$exe" "$seed" "$count" >/dev/null 2>&1 && ok=1
		fi

		if [ "$ok" = 1 ]; then
			pass=$((pass + 1))
			echo "  [corpus] OK: $run_desc -- $desc"
		else
			echo "  [corpus] FAIL: $run_desc REGRESSED (a pinned seed no longer passes on the clean build) -- $desc"
			fails=$((fails + 1))
		fi
	done < "$f"
done

echo "dst-corpus: $pass/$total pinned regression seeds pass on the clean build"
[ "$fails" -eq 0 ] || { echo "FAIL: $fails pinned corpus seed(s) regressed"; exit 1; }
[ "$total" -gt 0 ] || { echo "FAIL: corpus is empty"; exit 1; }
echo "OK: every pinned DST regression seed still passes (no silent regression)"
exit 0
