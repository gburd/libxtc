#!/usr/bin/env bash
# bench/sqlxtc/run.sh -- reproducible sweep of the sqlxtc MVCC engine.
#
# Copyright (c) 2026, The XTC Project -- ISC License.
#
# Builds libxtc + the MVCC load generator, then sweeps a matrix of core
# counts x read/write mixes, REPS times each, pinning to physical cores
# with taskset where available.  Emits one JSON object per run to
# results/<host>-<UTC>.jsonl for stats.py to aggregate.
#
# Usage:  ./run.sh [reps] [clients] [ops_per_client] [keyspace]
# Env:    CORES="1 2 4 8 16"   READ_PCTS="100 80 50 0"
#
# Measures the libxtc concurrency MODEL on an OLTP-ish KV workload.
# It is NOT a SQL TPC-C vs SQLite comparison: the SQL layer is not yet
# on this engine (see README.md).  No fabricated numbers -- it runs
# what is here and records exactly that.
set -euo pipefail
cd "$(dirname "$0")"

REPS="${1:-5}"
CLIENTS="${2:-32}"
OPS="${3:-20000}"
KEYSPACE="${4:-1000000}"
CORES="${CORES:-1 2 4 8}"
READ_PCTS="${READ_PCTS:-100 80 50 0}"

HOST="$(hostname -s 2>/dev/null || echo host)"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p results
OUT="results/${HOST}-${STAMP}.jsonl"

ROOT="$(cd ../.. && pwd)"
BUILD="$ROOT/build_unix"

# Build libxtc if needed (a host without build_unix builds it fresh).
if [ ! -f "$BUILD/libxtc.a" ]; then
	echo "building libxtc in $BUILD ..."
	mkdir -p "$BUILD"
	( cd "$BUILD" && "$ROOT/dist/configure" >/dev/null && make -j"$(nproc)" libxtc.a >/dev/null )
fi

CC="${CC:-gcc}"
LIBS="-pthread -ldl -lm"
# liburing is optional; link it only if the build used it.
if grep -q "luring\|HAVE_LIBURING" "$BUILD/Makefile" 2>/dev/null; then LIBS="$LIBS -luring"; fi

echo "building mvcc_bench ..."
$CC -std=c11 -O2 -g -D_GNU_SOURCE -I"$ROOT/src/inc" -I"$ROOT/examples/06_sqlxtc" \
	-o mvcc_bench mvcc_bench.c "$ROOT/examples/06_sqlxtc/mvcc.c" "$BUILD/libxtc.a" $LIBS

NCPU="$(nproc 2>/dev/null || echo 8)"
echo "host=$HOST cpus=$NCPU reps=$REPS clients=$CLIENTS ops=$OPS keyspace=$KEYSPACE"
echo "writing $OUT"

for rp in $READ_PCTS; do
	for c in $CORES; do
		[ "$c" -gt "$NCPU" ] && continue
		PIN=""
		command -v taskset >/dev/null 2>&1 && PIN="taskset -c 0-$((c-1))"
		for r in $(seq 1 "$REPS"); do
			line="$($PIN ./mvcc_bench --cores "$c" --clients "$CLIENTS" \
				--ops "$OPS" --keyspace "$KEYSPACE" --read-pct "$rp" \
				--label "r${rp}")"
			# annotate with host + rep
			echo "$line" | sed "s/}$/,\"host\":\"$HOST\",\"rep\":$r}/" >> "$OUT"
			echo "  rp=$rp cores=$c rep=$r: $(echo "$line" | grep -o '"kops_per_sec":[0-9.]*')"
		done
	done
done
echo "done -> $OUT"
echo "aggregate with:  python3 stats.py $OUT"
