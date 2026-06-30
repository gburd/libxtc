#!/bin/sh
# test/sim/run_sim_tests.sh
#
#	Build libxtc with the deterministic-simulation I/O backend
#	(--with-io-backend=sim) and run the DST tests that require it:
#	the multi-loop deterministic scheduler (test_sim_sched) and the
#	cross-loop park/wake replay (test_sim_pingpong).  These cannot run
#	in the normal (epoll/uring/kqueue) build because they drive the
#	executor through xtc_sim_exec_run on the sim backend.
#
#	The seeded-PRNG / virtual-clock unit test (test_sim_rng) runs in
#	the ordinary make check (sim is dormant there); this harness covers
#	the parts that need a sim build.
#
#	Each test asserts: a multi-loop run reaches quiescence on one
#	thread (no hang), and the SAME seed replays byte-identically while
#	a DIFFERENT seed reorders -- the core DST guarantee, exercised
#	against the REAL work-stealing executor and cross-loop parking.

set -eu

XTC_SRC_DIR="${XTC_SRC_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}"
CC="${CC:-cc}"
# Prefer an explicit TMPDIR; otherwise use /scratch/xtc-test only if it
# is usable (local convention), else fall back to the system default so
# CI runners without /scratch work unchanged.
if [ -z "${TMPDIR:-}" ]; then
	if mkdir -p /scratch/xtc-test 2>/dev/null; then
		TMPDIR=/scratch/xtc-test
		export TMPDIR
	fi
fi

if [ ! -x "$XTC_SRC_DIR/dist/configure" ]; then
	echo "  [sim] SKIP: dist/configure not generated"
	exit 0
fi

work="$(mktemp -d)"
trap 'cd / 2>/dev/null; rm -rf "$work"' EXIT INT TERM
build="$work/build"
mkdir -p "$build"

# Configure + build the sim-backend library.  No TLS / liburing: the sim
# backend has no platform deps.
(
	cd "$build"
	"$XTC_SRC_DIR/dist/configure" --with-io-backend=sim \
		--with-tls=none --without-liburing >/dev/null 2>&1
	make -j"$(nproc 2>/dev/null || echo 2)" >/dev/null 2>&1
) || { echo "  [sim] FAIL: sim-backend build errored"; exit 1; }

inc="-I$XTC_SRC_DIR/src/inc"
lib="$build/libxtc.a"
libs="-pthread -ldl -lm"

fail=0
for t in test_sim_sched test_sim_pingpong test_sim_fault test_sim_soak test_sim_critsec; do
	exe="$work/$t"
	# inc/libs intentionally word-split (each holds several flags).
	# shellcheck disable=SC2086
	if ! $CC -std=c11 -D_GNU_SOURCE $inc \
		"$XTC_SRC_DIR/test/sim/$t.c" "$lib" $libs -o "$exe" \
		2> "$work/cc.err"; then
		echo "  [sim] FAIL: $t did not compile"
		head -20 "$work/cc.err" >&2
		fail=1
		continue
	fi
	if "$exe" > "$work/out" 2>&1; then
		echo "  [sim] OK: $t -- $(grep '^OK' "$work/out" | head -1)"
	else
		echo "  [sim] FAIL: $t"
		cat "$work/out" >&2
		fail=1
	fi
done

if [ "$fail" -eq 0 ]; then
	echo "  [sim] OK: deterministic multi-loop scheduler + cross-loop"
	echo "        parking replay from seed (real work-stealing executor)"
fi
exit $fail
