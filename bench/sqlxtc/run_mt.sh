#!/bin/sh
# bench/sqlxtc/run_mt.sh -- multi-threaded A/B sweep (engine_mt.c):
# the same random point-op SQL workload run by T OS threads against the
# same storage, xstore (libxtc-native) vs SQLite (WAL), sweeping thread
# count (1..max cores), cache budget (RAM pressure), and read mix.
#
# Usage (from bench/sqlxtc inside the nix-shell):
#   nix-shell -p openssl pkg-config liburing util-linux \
#       --command 'sh run_mt.sh > results/engine_mt-$(hostname -s).jsonl'
set -eu

EX=../../examples/06_sqlxtc
XTC_BUILD=${XTC_BUILD:-../../build_unix}
ROWS=${ROWS:-1000000}            # ~ROWS*ROW_BYTES working set
ROW_BYTES=${ROW_BYTES:-200}
OPS=${OPS:-200000}               # per thread
REPS=${REPS:-3}
MAXT=${MAXT:-$(nproc)}
THREADS=${THREADS:-"1 2 4 8 16 $MAXT"}
# cache budgets: in-cache (fits ~200MB set) and larger-than-RAM (8MB)
CACHES=${CACHES:-"8192 262144"}
READMIX=${READMIX:-"100 95 50"}

[ -f "$EX/sqlite3.o" ] || make -C "$EX" XTC_BUILD="$XTC_BUILD" sqlite3.o >&2

gcc -O2 -g -std=c11 -D_GNU_SOURCE -include "$EX/xsql.h" -I"$EX" -I../../src/inc \
    engine_mt.c "$EX/xstore.c" "$EX/xlog.c" "$EX/wal.c" "$EX/btree.c" \
    "$EX/btnode.c" "$EX/bufmgr.c" "$EX/sqlite3.o" "$XTC_BUILD/libxtc.a" \
    -pthread -ldl -lm -luring -o engine_mt >&2

rep=1
while [ "$rep" -le "$REPS" ]; do
    for cache in $CACHES; do
        for rp in $READMIX; do
            for t in $THREADS; do
                for eng in xstore sqlite; do
                    ./engine_mt --engine "$eng" --threads "$t" --rows "$ROWS" \
                        --row-bytes "$ROW_BYTES" --cache-kb "$cache" --ops "$OPS" \
                        --read-pct "$rp" --label "rep$rep"
                done
            done
        done
    done
    rep=$((rep + 1))
done
