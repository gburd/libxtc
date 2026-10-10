#!/bin/sh
# Feature matrix and installed-header consumer; never skip missing APIs.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
"$root/dist/configure" --help | grep -q -- '--enable-sched-policy' || {
    echo 'FAIL: missing --enable-sched-policy'; exit 1;
}
for mode in no yes; do
    mkdir "$work/$mode"
    (cd "$work/$mode"
     "$root/dist/configure" --enable-sched-policy="$mode" \
         --with-tls=none --without-liburing --prefix="$work/install-$mode" >configure.log
     make -j2 >build.log
     make test_sched_policy test_sched_continuous test_batch >test-build.log
     ./test_sched_policy
     ./test_sched_continuous
     ./test_batch
     if [ "$mode" = yes ]; then
         make check-continuous-batching
     fi
     make install >install.log)
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
    "${CC:-cc}" $flag -I"$work/install-$mode/include" "$work/consumer.c" \
        "$work/install-$mode/lib/libxtc.a" -lpthread -lm -o "$work/consumer"
    "$work/consumer"
    # Compare actual stable-ID dispatch sequences, not just final state.
    mkdir "$work/sim-$mode"
    (cd "$work/sim-$mode"
     "$root/dist/configure" --enable-sched-policy="$mode" \
         --with-io-backend=sim --with-tls=none --without-liburing >configure.log
     make -j2 >build.log
     make test_sched_policy test_sched_continuous test_batch >test-build.log
     ./test_sched_policy
     ./test_sched_continuous
     ./test_batch)
    if [ "$mode" = yes ]; then
        "${CC:-cc}" -std=c11 -D_GNU_SOURCE -I"$work/sim-$mode" -I"$root/src/inc" \
            "$root/test/sim/test_sim_batch.c" "$work/sim-$mode/libxtc.a" \
            -lpthread -lm -o "$work/batch-dst"
        "$work/batch-dst" 1 64
    fi
    "${CC:-cc}" -I"$work/sim-$mode" -I"$root/src/inc" \
        "$root/test/sim/test_sched_policy_trace.c" \
        "$work/sim-$mode/libxtc.a" -lpthread -lm -o "$work/trace"
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
