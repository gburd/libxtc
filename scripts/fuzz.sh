#!/bin/sh
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# scripts/fuzz.sh NAME SECS -- build and run one libFuzzer harness.
#
#   NAME  net cfg tls sql quack resp kaka (test/fuzz/fuzz_NAME.c), or
#         "all" to run every harness one after another
#   SECS  seconds of fuzzing; 0 = regression only: run every input in the
#         committed corpus once and stop (what a PR job wants)
#
# Builds a coverage-instrumented libxtc with clang (ASan + UBSan) in
# FUZZ_BUILD (default build_fuzz in the source tree), then runs
# ./fuzz_NAME on a scratch copy of test/fuzz/corpus/NAME, so the
# committed seed corpus is never rewritten by a run.  A crash, a leak, a
# UBSan report or a failed harness invariant stops the run and leaves the
# reproducer in FUZZ_OUT/NAME/artifacts; the exit status is non-zero.
#
# To pin a crash as a regression input: fix the bug, copy the artifact to
# test/fuzz/corpus/NAME/crash-<what>, and commit both (a later run fails
# if it regresses, because every corpus input runs first).
#
# env: FUZZ_BUILD (build dir), FUZZ_OUT (default FUZZ_BUILD/fuzz-out),
#      FUZZ_CONFIGURE (extra configure args, default --with-tls=auto; e.g.
#      --with-tls=mbedtls to fuzz that backend's ALPN decoder),
#      FUZZ_ARGS (extra libFuzzer flags, e.g. -fork=4), CC (default clang),
#      FUZZ_KEEP=1 to keep the scratch corpus (the inputs the run found).
set -eu

name=${1:?usage: fuzz.sh NAME SECS}
secs=${2:?usage: fuzz.sh NAME SECS}
# shellcheck disable=SC1007
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
B=${FUZZ_BUILD:-$src/build_fuzz}
CC=${CC:-clang}
J=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)

if [ "$name" = all ]; then
	rc=0
	for n in net cfg tls sql quack resp kaka; do
		"$0" "$n" "$secs" || rc=1
	done
	exit $rc
fi
if [ ! -f "$src/test/fuzz/fuzz_$name.c" ]; then
	echo "fuzz.sh: no harness test/fuzz/fuzz_$name.c" >&2
	exit 2
fi
if ! command -v "$CC" >/dev/null 2>&1; then
	echo "fuzz.sh: SKIP: $CC not found (libFuzzer needs clang)" >&2
	exit 77
fi

mkdir -p "$B"
if [ ! -f "$B/Makefile" ]; then
	if [ ! -x "$src/dist/configure" ]; then
		( cd "$src/dist" && autoreconf -i ) >/dev/null
	fi
	# shellcheck disable=SC2086  # FUZZ_CONFIGURE is a word list
	( cd "$B" && CC="$CC" \
	    CFLAGS="-g -O1 -fno-omit-frame-pointer -fsanitize=fuzzer-no-link,address,undefined" \
	    LDFLAGS="-fsanitize=address,undefined" \
	    "$src/dist/configure" ${FUZZ_CONFIGURE:---with-tls=auto} ) \
	    > "$B/fuzz-configure.log" 2>&1 || {
		echo "fuzz.sh: configure failed; see $B/fuzz-configure.log" >&2
		exit 2
	}
fi
make -s -C "$B" -j"$J" libxtc.a > "$B/fuzz-build.log" 2>&1 &&
    make -s -C "$B" "fuzz_$name" >> "$B/fuzz-build.log" 2>&1 || {
	echo "fuzz.sh: build failed; see $B/fuzz-build.log" >&2
	tail -20 "$B/fuzz-build.log" >&2
	exit 2
}

out=${FUZZ_OUT:-$B/fuzz-out}/$name
seeds=$src/test/fuzz/corpus/$name
mkdir -p "$out/artifacts" "$out/corpus"
dict=
[ -f "$src/test/fuzz/dict/$name.dict" ] && dict="-dict=$src/test/fuzz/dict/$name.dict"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"

cd "$B"
if [ "$secs" = 0 ]; then
	# Regression: each committed input once.
	set -- "$seeds"/*
	[ -e "$1" ] || { echo "fuzz.sh: empty corpus $seeds" >&2; exit 2; }
	echo "fuzz.sh: $name: regression over $# corpus input(s)"
	# shellcheck disable=SC2086
	./"fuzz_$name" -artifact_prefix="$out/artifacts/" $dict "$@" \
	    > "$out/run.log" 2>&1 || {
		tail -40 "$out/run.log"
		echo "fuzz.sh: $name: FAIL (corpus regression; log $out/run.log)"
		exit 1
	}
	echo "fuzz.sh: $name: OK ($# inputs)"
	exit 0
fi
echo "fuzz.sh: $name: fuzzing for $secs s (seeds $seeds)"
rc=0
# shellcheck disable=SC2086  # FUZZ_ARGS and dict are word lists
./"fuzz_$name" -max_total_time="$secs" -timeout=10 -rss_limit_mb=2048 \
    -print_final_stats=1 -artifact_prefix="$out/artifacts/" $dict \
    ${FUZZ_ARGS:-} "$out/corpus" "$seeds" > "$out/run.log" 2>&1 || rc=$?
grep -E '^stat::(number_of_executed_units|new_units_added|peak_rss_mb)' \
    "$out/run.log" | tr '\n' ' '
echo
if [ "$rc" -ne 0 ]; then
	grep -E 'ERROR|SUMMARY|runtime error|deadly signal|Test unit written' \
	    "$out/run.log" | head -10
	echo "fuzz.sh: $name: FAIL rc=$rc (artifacts in $out/artifacts; log $out/run.log)"
	exit 1
fi
[ "${FUZZ_KEEP:-0}" = 1 ] || find "$out/corpus" -type f -delete
echo "fuzz.sh: $name: OK"
