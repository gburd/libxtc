#!/bin/sh
#-
# Copyright (c) 2026, The XTC Project — All rights reserved.
# Use of this source code is governed by the ISC License.
#
# test/dist/test_m17_smoke.sh — M17 smoke test
#
# Verify that bench/conformance/run.sh:
#   1. Exits 0 when no benchmark binaries are present.
#   2. Emits exactly the CSV header line on stdout (no data rows).
#
# This test does NOT build or run any workload binaries; it only confirms
# that the infrastructure script is self-consistent on an empty scaffold.
#
set -eu
: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set}"

RUN_SH="$XTC_SRC_DIR/bench/conformance/run.sh"

if [ ! -f "$RUN_SH" ]; then
    printf '  [M17-smoke] FAIL: %s not found\n' "$RUN_SH" >&2
    exit 1
fi

EXPECTED="workload,runtime,params,elapsed_ns,cpu_us,rss_kb,p50_ns,p95_ns,p99_ns,p999_ns"

# Run with stderr suppressed; capture stdout.
ACTUAL=$(sh "$RUN_SH" 2>/dev/null)

if [ "$ACTUAL" != "$EXPECTED" ]; then
    printf '  [M17-smoke] FAIL: unexpected stdout from run.sh\n' >&2
    printf '  expected: %s\n' "$EXPECTED" >&2
    printf '  got:      %s\n' "$ACTUAL" >&2
    exit 1
fi

printf '  [M17-smoke] OK: run.sh on empty workloads produces header-only CSV\n'
