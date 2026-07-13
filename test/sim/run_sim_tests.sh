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
	make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)" >/dev/null 2>&1
) || { echo "  [sim] FAIL: sim-backend build errored"; exit 1; }

inc="-I$XTC_SRC_DIR/src/inc"
lib="$build/libxtc.a"
case "$(uname -s)" in
Darwin) libs="-pthread -lm" ;;
*)      libs="-pthread -ldl -lm" ;;
esac

# test_sim_bufmgr drives the sqlxtc storage-engine buffer manager
# (examples/06_sqlxtc/bufmgr.c) under DST, so it needs that source
# compiled in plus its header include.  bufmgr.c depends only on the
# library (xtc_aio / xtc_stats / xtc_sync / xtc_dio_sched / os_time),
# which the sim lib provides -- no other examples/06_sqlxtc object is
# needed.  Compile it against the sim lib into a per-test object.
bmdir="$XTC_SRC_DIR/examples/06_sqlxtc"
bmobj="$work/bufmgr.o"
# shellcheck disable=SC2086
if ! $CC -std=c11 -D_GNU_SOURCE $inc -I"$bmdir" \
	-c "$bmdir/bufmgr.c" -o "$bmobj" 2> "$work/cc.err"; then
	echo "  [sim] FAIL: bufmgr.c (for test_sim_bufmgr) did not compile"
	head -20 "$work/cc.err" >&2
	exit 1
fi

# test_sim_crash_recover drives the WHOLE native SQL engine (sx_* over
# the shared B-tree) + the WAL + recovery under DST, so it needs the
# ENGINE_NATIVE object set from examples/06_sqlxtc (the same set the
# non-sim test_wal_recover links).  These depend only on the library
# (which the sim lib provides) + the SQLXTC_HAVE_LIME parser, so compile
# each against the sim lib into per-test objects (bufmgr.o is reused).
engobjs="$bmobj"
for s in engine db quack metrics vexec sql_parse sql_parse_drv sql_ast \
	sql_parse_gen xstore xlog wal btree btnode; do
	o="$work/$s.o"
	# shellcheck disable=SC2086
	if ! $CC -std=c11 -D_GNU_SOURCE -DSQLXTC_HAVE_LIME=1 $inc -I"$bmdir" \
		-c "$bmdir/$s.c" -o "$o" 2> "$work/cc.err"; then
		echo "  [sim] FAIL: $s.c (for test_sim_crash_recover) did not compile"
		head -20 "$work/cc.err" >&2
		exit 1
	fi
	engobjs="$engobjs $o"
done

fail=0
for t in test_sim_sched test_sim_pingpong test_sim_wake_park test_sim_fault test_sim_soak test_sim_critsec test_sim_latch test_sim_lockmgr test_sim_lwlock test_sim_lrlock test_sim_rcu test_sim_iofault test_sim_torn test_sim_stale test_sim_buggify test_sim_buggify2 test_sim_buggify3 test_sim_buggify4 test_sim_partition test_sim_machine_death test_sim_svr test_sim_fsm test_sim_credit test_sim_pool test_sim_pg test_sim_proc_teardown test_sim_xproc test_sim_chan test_sim_sync test_sim_reg test_sim_mctx test_sim_slab test_sim_pdict test_sim_stats test_sim_tnt test_sim_exit_teardown test_sim_spawn_rel test_sim_determinism test_sim_launch test_sim_aiov test_sim_sup_strategy test_sim_app test_sim_blocking test_sim_osproc test_sim_res test_sim_stream test_sim_swarm test_sim_bufmgr test_sim_crash_recover test_sim_compose test_sim_compose_crash; do
	exe="$work/$t"
	# test_sim_bufmgr additionally needs the bufmgr object + its include;
	# test_sim_crash_recover needs the whole native engine object set.
	extra_obj=""
	extra_inc=""
	if [ "$t" = test_sim_bufmgr ]; then
		extra_obj="$bmobj"
		extra_inc="-I$bmdir"
	elif [ "$t" = test_sim_crash_recover ] || [ "$t" = test_sim_compose ] || [ "$t" = test_sim_compose_crash ]; then
		extra_obj="$engobjs"
		extra_inc="-I$bmdir -DSQLXTC_HAVE_LIME=1"
	fi
	# inc/libs intentionally word-split (each holds several flags).
	# shellcheck disable=SC2086
	if ! $CC -std=c11 -D_GNU_SOURCE $inc $extra_inc \
		"$XTC_SRC_DIR/test/sim/$t.c" $extra_obj "$lib" $libs -o "$exe" \
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
