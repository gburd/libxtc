#!/bin/sh
#-
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# test/dist/test_m17_smoke.sh -- M17 smoke test
#
# Verify that bench/conformance/run.sh:
#   1. Exits 0.
#   2. Emits the correct CSV header as the first stdout line.
#   3. Any data rows that follow contain the correct number of comma-
#      separated fields (10) and a recognised workload= token.
#
# This test does NOT build any workload binaries.  If the W1/xtc binary
# has already been compiled it will appear in the output; that is expected
# and the test validates its format.  If no binaries are present, only the
# header row is emitted and that is also fine.
#
set -eu
: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set}"

RUN_SH="$XTC_SRC_DIR/bench/conformance/run.sh"

if [ ! -f "$RUN_SH" ]; then
    printf '  [M17-smoke] FAIL: %s not found\n' "$RUN_SH" >&2
    exit 1
fi

EXPECTED_HEADER="workload,runtime,params,elapsed_ns,cpu_us,rss_kb,p50_ns,p95_ns,p99_ns,p999_ns"

# Run with stderr suppressed; capture stdout.
OUTPUT=$(sh "$RUN_SH" 2>/dev/null)

# 1. First line must be the CSV header.
FIRST_LINE=$(printf '%s\n' "$OUTPUT" | head -1)
if [ "$FIRST_LINE" != "$EXPECTED_HEADER" ]; then
    printf '  [M17-smoke] FAIL: first line is not the expected CSV header\n' >&2
    printf '  expected: %s\n' "$EXPECTED_HEADER" >&2
    printf '  got:      %s\n' "$FIRST_LINE" >&2
    exit 1
fi

# 2. Every data row (lines 2+) must have exactly 10 comma-separated fields
#    and begin with a known workload= token.
_row=0
printf '%s\n' "$OUTPUT" | tail -n +2 | while IFS= read -r _line; do
    [ -n "$_line" ] || continue
    _row=$((_row + 1))
    # Count commas: 9 commas -> 10 fields.
    _commas=$(printf '%s' "$_line" | tr -cd ',' | wc -c)
    if [ "$_commas" -ne 9 ]; then
        printf '  [M17-smoke] FAIL: data row %d has %d commas (want 9): %s\n' \
            "$_row" "$_commas" "$_line" >&2
        exit 1
    fi
    # First field must start with "W" (e.g. W1..W7).
    _wl=$(printf '%s' "$_line" | cut -d',' -f1)
    case "$_wl" in
        W[1-7]) ;;
        *)
            printf '  [M17-smoke] FAIL: data row %d has unexpected workload %s\n' \
                "$_row" "$_wl" >&2
            exit 1
            ;;
    esac
done

printf '  [M17-smoke] OK: run.sh emits valid CSV (header + %d data row(s))\n' \
    "$(printf '%s\n' "$OUTPUT" | tail -n +2 | grep -c '[^[:space:]]' 2>/dev/null || printf 0)"
