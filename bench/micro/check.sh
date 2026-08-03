#!/bin/sh
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# bench/micro/check.sh -- the microbenchmark REGRESSION GATE.
#
# Runs the suite, compares each benchmark's median ns/op against the
# committed baseline, and EXITS NONZERO if any benchmark regressed
# beyond its tolerance (printing which, and by how much).  This is the
# release gate: a hot path that got materially slower fails the build.
#
# Baselines are MACHINE-RELATIVE -- the same hardware, compiler, and
# build flags.  Run this on a CONSISTENT host (see README.md).  The
# tolerance is deliberately generous (default +100%, i.e. catch a hot
# path that DOUBLED) because microbench variance on a shared/loaded host
# is large and one-sided; the value is catching a BIG regression (a hot
# path going ~2x slower), not micro-noise.
#
# Usage:
#   check.sh [baseline.json] [scale]
# Env:
#   BENCH_MICRO_BIN   path to the bench_micro binary (default: ./bench_micro)
#   BENCH_TOLERANCE   fractional slowdown allowed, e.g. 1.0 = +100% (2x)
#                     (default: baseline file's per-benchmark "tolerance"
#                      or the file's top-level "default_tolerance")
#   BENCH_UPDATE      if "1", instead of checking, WRITE a fresh baseline
#                     to the baseline path (used to generate baseline.json)
#
# Exit: 0 = all within tolerance (or baseline written); 1 = a regression;
# 2 = a setup/run error.

set -eu

# shellcheck disable=SC1007  # `CDPATH= cd` clears CDPATH for this cd only
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
baseline="${1:-$here/baseline.json}"
scale="${2:-1}"

csv=$(mktemp)
trap 'rm -f "$csv"' EXIT INT TERM

# run.sh writes the CSV; capture it (and surface a run failure as exit 2).
if ! "$here/run.sh" "$scale" >"$csv"; then
	echo "check.sh: benchmark run failed" >&2
	exit 2
fi

BENCH_UPDATE="${BENCH_UPDATE:-0}" \
BENCH_TOLERANCE="${BENCH_TOLERANCE:-}" \
	python3 "$here/check.py" "$baseline" "$csv"
