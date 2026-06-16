#!/bin/sh
# bench/sqlxtc/run_vexec.sh -- build and run the vexec A/B benchmark:
# the same analytic query through SQLite's VDBE, the single-threaded
# vectorized executor (vexec), and the morsel-parallel vexec at 1/2/4/8
# worker loops.  Shows the V2/V3 parallelism on scan/filter/aggregate/
# join shapes.
#
# Usage (from bench/sqlxtc inside the nix-shell):
#   nix-shell -p openssl pkg-config liburing \
#       --command 'sh run_vexec.sh 2000000 | tee results/vexec-$(hostname -s).txt'
set -eu

EX=../../examples/06_sqlxtc
XTC_BUILD=${XTC_BUILD:-../../build_unix}
ROWS=${1:-2000000}

# The Lime parser must be generated (committed copy is fine) and the
# vendored engine compiled with the xsql_ rename.
[ -f "$EX/sqlite3.o" ] || make -C "$EX" XTC_BUILD="$XTC_BUILD" sqlite3.o >&2
[ -f "$EX/sql_parse_gen.c" ] || make -C "$EX" sql_parse_gen.c >&2

gcc -O2 -g -std=c11 -D_GNU_SOURCE -include "$EX/xsql.h" -I"$EX" -I../../src/inc \
    vexec_bench.c "$EX/vexec.c" "$EX/sql_parse_drv.c" "$EX/sql_ast.c" \
    "$EX/sql_parse_gen.c" "$EX/xstore.c" "$EX/xlog.c" "$EX/wal.c" \
    "$EX/btree.c" "$EX/btnode.c" "$EX/bufmgr.c" "$EX/sqlite3.o" \
    "$XTC_BUILD/libxtc.a" -pthread -ldl -lm -luring -o vexec_bench >&2

./vexec_bench "$ROWS"
