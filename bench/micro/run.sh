#!/bin/sh
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# bench/micro/run.sh -- run the microbenchmark suite and emit its
# machine-readable CSV to stdout.
#
# The driver (bench_micro) already prints CSV; this wrapper just locates
# the binary and passes through an optional iteration-scale argument.
#
# Usage:
#   run.sh [scale]
# Env:
#   BENCH_MICRO_BIN   path to the bench_micro binary (default: ./bench_micro)
#
# Output (stdout): a leading `#`-comment line, a `name,ns_per_op,ops_per_sec`
# header, then one CSV row per benchmark.  check.sh consumes this.

set -eu

bin="${BENCH_MICRO_BIN:-./bench_micro}"
scale="${1:-1}"

if [ ! -x "$bin" ]; then
	echo "run.sh: benchmark binary not found or not executable: $bin" >&2
	echo "run.sh: build it first (make bench_micro) or set BENCH_MICRO_BIN" >&2
	exit 2
fi

exec "$bin" "$scale"
