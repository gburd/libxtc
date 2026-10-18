#!/bin/sh
# Feature matrix: prove the --enable-sched-policy build works off AND on,
# that a disabled build reports NOSYS, that the installed headers compile,
# and that the dispatch-trace replays identically off vs on.  Deliberately
# LEAN: the heavy test suites (test_batch, check-continuous-batching, the
# full make check / check-dst) run in the dedicated CI jobs, NOT here --
# this matrix would otherwise rebuild the whole library four times with
# every test and blow the CI step budget.  Here we build the library once
# per configuration (at full parallelism) and only the small binaries this
# matrix's own checks need.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
jobs=$(nproc 2>/dev/null || echo 2)

"$root/dist/configure" --help | grep -q -- '--enable-sched-policy' || {
    echo 'FAIL: missing --enable-sched-policy'; exit 1;
}

for mode in no yes; do
    # --- native build: compile-equivalence + installed-header consumer ---
    mkdir "$work/$mode"
    (cd "$work/$mode"
     "$root/dist/configure" --enable-sched-policy="$mode" \
         --with-tls=none --without-liburing --prefix="$work/install-$mode" >configure.log
     make -j"$jobs" libxtc.a >build.log 2>&1
     # Build the small feature binaries (compiles the new APIs in both
     # modes); run only the quick policy/continuous checks, not the heavy
     # test_batch (that runs in the dedicated make check job).
     make -j"$jobs" test_sched_policy test_sched_continuous >test-build.log 2>&1
     ./test_sched_policy
     ./test_sched_continuous
     make install >install.log 2>&1)

    cat > "$work/consumer.c" <<'SRC'
#include <xtc.h>
#include <xtc_sched.h>
#include <xtc_batch.h>
int main(void) {
    xtc_loop_t *loop;
    int rc, batch_rc;
    if (xtc_loop_init(&loop) != XTC_OK) return 1;
    rc = xtc_sched_continuous_set(loop, 3);
    batch_rc = xtc_batch_create(loop, 0, 0);
    xtc_loop_fini(loop);
#ifdef EXPECT_OFF
    return rc != XTC_E_NOSYS || batch_rc != XTC_E_NOSYS;
#else
    return rc != XTC_OK || batch_rc != XTC_E_INVAL;
#endif
}
SRC
    flag=
    [ "$mode" = yes ] || flag=-DEXPECT_OFF
    # Plus the libraries configure found libxtc.a needs (FreeBSD: -lrt).
    libs=$(sed -n 's/^LIBS[[:space:]]*=[[:space:]]*//p' "$work/$mode/Makefile")
    # shellcheck disable=SC2086  # libs holds several flags
    "${CC:-cc}" $flag -I"$work/install-$mode/include" "$work/consumer.c" \
        "$work/install-$mode/lib/libxtc.a" -lpthread -lm $libs -o "$work/consumer"
    "$work/consumer"

    # --- sim build: dispatch-trace replay-equivalence + batch replay ---
    mkdir "$work/sim-$mode"
    (cd "$work/sim-$mode"
     "$root/dist/configure" --enable-sched-policy="$mode" \
         --with-io-backend=sim --with-tls=none --without-liburing >configure.log
     make -j"$jobs" libxtc.a >build.log 2>&1)
    simlibs=$(sed -n 's/^LIBS[[:space:]]*=[[:space:]]*//p' "$work/sim-$mode/Makefile")
    if [ "$mode" = yes ]; then
        # One deterministic async+mt replay (the feature's own DST check);
        # the full sim suite runs in the sim-dst job.
        "${CC:-cc}" -std=c11 -D_GNU_SOURCE -I"$work/sim-$mode" -I"$root/src/inc" \
            "$root/test/sim/test_sim_batch.c" "$work/sim-$mode/libxtc.a" \
            -lpthread -ldl -lm $simlibs -o "$work/batch-dst"
        "$work/batch-dst" 1 64
    fi
    "${CC:-cc}" -I"$work/sim-$mode" -I"$root/src/inc" \
        "$root/test/sim/test_sched_policy_trace.c" \
        "$work/sim-$mode/libxtc.a" -lpthread -ldl -lm $simlibs -o "$work/trace"
    "$work/trace" > "$work/trace-$mode"
    "$work/trace" > "$work/replay-$mode"
    cmp "$work/trace-$mode" "$work/replay-$mode"
done
cmp "$work/trace-no" "$work/trace-yes"

mkdir "$work/bad"
if (cd "$work/bad"; "$root/dist/configure" --enable-sched-policy=invalid >log 2>&1); then
    echo 'FAIL: accepted invalid sched-policy value'; exit 1
fi
echo '[sched-policy] OK'
