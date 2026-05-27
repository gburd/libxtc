#!/bin/sh
#-
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# bench/conformance/run.sh
#   Discover and run all conformance benchmark binaries, then aggregate
#   their output into a single CSV.
#
# Usage:
#   ./bench/conformance/run.sh [--out=FILE]
#
# Default: CSV lines are written to stdout; progress and the final
# summary are written to stderr.  Redirect with --out=FILE if you
# want the CSV in a file rather than on stdout.
#
# Binary discovery:
#   For each workload directory  bench/conformance/w<N>_<name>/
#   and each runtime in {xtc, tokio, erlang}, the script looks for an
#   executable named "bench" at  w<N>_<name>/<runtime>/bench.
#   If the executable is absent or not executable, the (workload, runtime)
#   pair is silently skipped.
#
# Output format (per run):
#   Each binary must write exactly one key=value line to stdout; see
#   docs/M17_RESULTS_FORMAT.md for the full specification.
#
# CSV columns produced:
#   workload, runtime, params, elapsed_ns, cpu_us, rss_kb,
#   p50_ns, p95_ns, p99_ns, p999_ns
#
# Requirements: POSIX sh; no external utilities beyond what is in
# /usr/bin on any POSIX system.

set -eu
LC_ALL=C
export LC_ALL

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
SELF_DIR=$(cd "$(dirname "$0")" && pwd)
_out=""

for _arg in "$@"; do
    case "$_arg" in
        --out=*)
            _out="${_arg#--out=}"
            ;;
        *)
            printf 'run.sh: unknown argument: %s\n' "$_arg" >&2
            exit 1
            ;;
    esac
done

# If --out was given, redirect stdout to that file.
if [ -n "$_out" ]; then
    exec >"$_out"
fi

# ---------------------------------------------------------------------------
# CSV header
# ---------------------------------------------------------------------------
printf 'workload,runtime,params,elapsed_ns,cpu_us,rss_kb,p50_ns,p95_ns,p99_ns,p999_ns\n'

# ---------------------------------------------------------------------------
# Temp file for capturing binary output (avoids subshell / IFS issues)
# ---------------------------------------------------------------------------
_tmpfile="${TMPDIR:-/tmp}/xtc_conf_$$.tmp"

# Remove temp file on exit (normal and signal-induced).
# shellcheck disable=SC2064
trap "rm -f '$_tmpfile'" EXIT

_n_pairs=0

# ---------------------------------------------------------------------------
# Workload loop
# ---------------------------------------------------------------------------
for _wdir in "$SELF_DIR"/w[0-9]_*/; do
    [ -d "$_wdir" ] || continue

    for _rt in xtc tokio erlang; do
        _rtdir="${_wdir}${_rt}"
        [ -d "$_rtdir" ] || continue

        _bin="${_rtdir}/bench"
        [ -x "$_bin" ] || continue

        printf 'run.sh: running %s\n' "$_bin" >&2

        # Run; skip this pair if the binary exits non-zero.
        if ! "$_bin" >"$_tmpfile" 2>/dev/null; then
            printf 'run.sh: %s exited non-zero -- skipped\n' "$_bin" >&2
            continue
        fi

        # Parse each output line into CSV columns.
        while IFS= read -r _line; do
            [ -n "$_line" ] || continue

            _workload=""
            _runtime=""
            _params=""
            _elapsed_ns="0"
            _cpu_us="0"
            _rss_kb="0"
            _p50_ns="0"
            _p95_ns="0"
            _p99_ns="0"
            _p999_ns="0"

            # shellcheck disable=SC2086
            for _field in $_line; do
                _key="${_field%%=*}"
                _val="${_field#*=}"
                case "$_key" in
                    workload)   _workload="$_val"   ;;
                    runtime)    _runtime="$_val"    ;;
                    params)     _params="$_val"     ;;
                    elapsed_ns) _elapsed_ns="$_val" ;;
                    cpu_us)     _cpu_us="$_val"     ;;
                    rss_kb)     _rss_kb="$_val"     ;;
                    p50_ns)     _p50_ns="$_val"     ;;
                    p95_ns)     _p95_ns="$_val"     ;;
                    p99_ns)     _p99_ns="$_val"     ;;
                    p999_ns)    _p999_ns="$_val"    ;;
                esac
            done

            # Only emit a row if we got at least a workload tag.
            [ -n "$_workload" ] || continue

            printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
                "$_workload" "$_runtime" "$_params" \
                "$_elapsed_ns" "$_cpu_us" "$_rss_kb" \
                "$_p50_ns" "$_p95_ns" "$_p99_ns" "$_p999_ns"
        done <"$_tmpfile"

        _n_pairs=$((_n_pairs + 1))
    done
done

printf 'run.sh: %d (workload,runtime) pair(s) run\n' "$_n_pairs" >&2
