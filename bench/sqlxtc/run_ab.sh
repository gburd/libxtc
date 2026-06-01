#!/bin/sh
# bench/sqlxtc/run_ab.sh -- build and run the larger-than-RAM A/B
# (engine_ab.c): the same SQL workload through the same VDBE, differing
# only in the storage engine (xstore = libxtc-native, vs SQLite's own).
#
# Usage: run from bench/sqlxtc inside the nix-shell:
#   nix-shell -p openssl pkg-config liburing util-linux \
#       --command 'sh run_ab.sh > results/engine_ab-$(hostname -s).jsonl'
set -eu

EX=../../examples/06_sqlxtc
XTC_BUILD=${XTC_BUILD:-../../build_unix}
CPU=${CPU:-2}                      # core to pin to (taskset)
ROWS=${ROWS:-200000}
ROW_BYTES=${ROW_BYTES:-512}
CACHE_KB=${CACHE_KB:-4096}
OPS=${OPS:-100000}
REPS=${REPS:-3}

# The vendored engine compiled with the xsql_ rename (force-include).
[ -f "$EX/sqlite3.o" ] || make -C "$EX" XTC_BUILD="$XTC_BUILD" sqlite3.o >&2

gcc -O2 -g -std=c11 -D_GNU_SOURCE -include "$EX/xsql.h" -I"$EX" -I../../src/inc \
    engine_ab.c "$EX/xstore.c" "$EX/btree.c" "$EX/btnode.c" "$EX/bufmgr.c" \
    "$EX/sqlite3.o" "$XTC_BUILD/libxtc.a" -pthread -ldl -lm -luring \
    -o engine_ab >&2

run() {
    if command -v taskset >/dev/null 2>&1; then
        taskset -c "$CPU" "$@"
    else
        "$@"
    fi
}

rep=1
while [ "$rep" -le "$REPS" ]; do
    for rp in 100 95 50; do
        for eng in xstore sqlite; do
            run ./engine_ab --engine "$eng" --rows "$ROWS" --row-bytes "$ROW_BYTES" \
                --cache-kb "$CACHE_KB" --ops "$OPS" --read-pct "$rp" --label "rep$rep"
        done
    done
    rep=$((rep + 1))
done
