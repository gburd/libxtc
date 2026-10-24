#!/bin/sh
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# scripts/uring-gate.sh -- check how well libxtc's tests and examples use
# io_uring, with uringscope (https://github.com/rch0wdhury/uringscope; it
# is in the nix dev shell).  uringscope traces the kernel's io_uring
# tracepoints and reports, per run: SQEs per io_uring_enter, which
# requests fell off the fast path onto io-wq worker threads ("punts"), CQ
# overflows, errors, requests that never completed, and (with --check)
# overlapping in-flight buffers.
#
# Each workload below runs under `uringscope --json`.  The verdict per
# workload is one of:
#   ok        no finding at WARN or above, and io_uring was observed
#   EXPECTED  only findings this table says the workload is allowed
#   FAIL      any other WARN/CRIT finding (OVERFLOW, LEAK, ERRORS, HAZARD,
#             an unexpected PUNT or WORKERS) -- exit 1
#   NODATA    io_uring was not observed at all: the run proves nothing.
#             A workload that is supposed to use io_uring and shows
#             NODATA is a FAIL (it may have picked another backend, or
#             uringscope attached after it finished: rerun longer)
# INFO findings (BATCH, SHORT-WRITE) are reported, never fail: BATCH is
# libxtc's known gap (each xtc_aio_* submits its own SQE; see
# docs/KNOWN_ISSUES.md), and the counters are printed so a release can
# show it moving.
#
# usage: sudo scripts/uring-gate.sh BUILD_DIR [OUTDIR]
#   BUILD_DIR: a configured, built libxtc tree (dist/configure; io_uring
#   backend).  The examples (sqlxtc, kaka) are built against it here.  Needs root (or CAP_BPF + CAP_PERFMON) and a BTF kernel.
#   CHECK=1 also runs each workload under --check (buffer hazards; slower).
# exit: 0 clean, 1 a FAIL, 2 setup error.

set -eu

build=${1:?usage: uring-gate.sh BUILD_DIR [OUTDIR]}
out=${2:-${TMPDIR:-/tmp}/uring-gate.$$}
# shellcheck disable=SC1007
build=$(CDPATH= cd -- "$build" && pwd)
# shellcheck disable=SC1007
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$out"
scratch="$out/tmp"
mkdir -p "$scratch"

command -v uringscope > /dev/null 2>&1 || {
	echo "uring-gate: uringscope not on PATH (nix develop, or" \
	    "https://github.com/rch0wdhury/uringscope/releases)" >&2
	exit 2
}
command -v jq > /dev/null 2>&1 || { echo "uring-gate: needs jq" >&2; exit 2; }
[ -r /sys/kernel/btf/vmlinux ] || { echo "uring-gate: no BTF kernel" >&2; exit 2; }
[ "$(id -u)" = 0 ] || echo "uring-gate: not root; uringscope needs CAP_BPF + CAP_PERFMON" >&2
grep -q '^#define XTC_IO_BACKEND_URING 1' "$build/xtc_config.h" 2>/dev/null || {
	echo "uring-gate: $build is not built with the io_uring backend" \
	    "(configure --with-io-backend=uring)" >&2
	exit 2
}

sx="$src/examples/06_sqlxtc"
(
	cd "$build"
	make bench_aio_fibers bench_exec_scale > "$out/build.log" 2>&1 || true
	cd "$sx"
	make XTC_BUILD="$build" test_wal_kill test_crash_oracle \
	    test_wal_group_sync sqlxtc-server >> "$out/build.log" 2>&1 || true
	cd "$src/examples/07_kaka"
	make XTC_BUILD="$build" kaka-broker kaka_soak \
	    >> "$out/build.log" 2>&1 || true
)
dat="$scratch/aio.dat"
dd if=/dev/zero of="$dat" bs=1M count=256 status=none

# ---- workloads -------------------------------------------------------
# name | allowed findings (tags, comma-separated) | command
# The allowances are the documented, understood cases (each measured with
# plain liburing as a control, on Linux 6.12 / XFS):
#   aio_fsync: FSYNC always runs on io-wq (an io_uring property).
#   aio_write: buffered writes to ONE file from several rings punt when
#     they contend on the inode lock.  One ring writing an existing file
#     punts 0.4%; 4 threads with a ring each 36%; 16 threads 97% -- and
#     bench_aio_fibers runs 16 loops on one file.  Hence PUNT and WORKERS.
#   crash_oracle: buffered page writes punt when they extend or fill a
#     hole in the page file (XFS allocates on io-wq).  sqlxtc's WAL
#     commits do not (O_DSYNC + preallocated log): wal_kill shows <1%.
#   kaka_soak, sqlxtc_iosoak: the concurrent network + storage soaks.  A
#     command starting with "@wrap " is a test script that runs its
#     SERVER under $SOAK_WRAP, so uringscope traces the server only (the
#     clients' own socket and disk I/O stay out of the report).  kaka:
#     FSYNC always punts, segment appends extend a new file (WRITE punts),
#     and kaka raises the bounded io-wq cap to 64 on purpose
#     (MAX_SYNC_WORKERS in examples/07_kaka/main.c), hence WORKERS.
#     sqlxtc: its evicting buffer pool writes pages back and extends the
#     page file (WRITE/WRITEV punt).
# ERRORS is judged per opcode: a cancelled POLL_ADD completes -ECANCELED
# and uringscope counts it as an error; libxtc cancels polls routinely
# (one-shot re-arm, unregister).  Errors on any other opcode fail.
# Anything else at WARN or above fails.
workloads() {
	cat <<EOF
aio_read||$build/bench_aio_fibers $dat 64 4000 4096
aio_write|PUNT,WORKERS|$build/bench_aio_fibers $dat 64 4000 4096 write
aio_fsync|PUNT|$build/bench_aio_fibers $dat 16 200 4096 fdatasync
exec_scale||$build/bench_exec_scale 8 2000000 spawn
wal_kill||$sx/test_wal_kill
crash_oracle|PUNT|$sx/test_crash_oracle 4 1
rexis||make -C $build check-rexis
kaka_soak|PUNT,WORKERS|@wrap env KAKA_BROKER=$src/examples/07_kaka/kaka-broker KAKA_SOAK=$src/examples/07_kaka/kaka_soak SECS=15 bash $src/examples/07_kaka/test_kaka_soak.sh
sqlxtc_iosoak|PUNT|@wrap env SQLXTC_SERVER=$sx/sqlxtc-server SECS=20 CLIENTS=32 bash $src/test/sqlxtc/test_sqlxtc_iosoak.sh
EOF
}

rm -f "$out/.fail"
printf '%-14s %-9s %10s %10s %8s %7s  %s\n' workload verdict sqes enters \
    sqe/enter punt% findings
workloads | while IFS='|' read -r name allow cmd; do
	j="$out/$name.json"
	case $cmd in
	"@wrap "*)
		# the script wraps its own server: trace that process tree only;
		# a failing soak (bad data, a crash) fails the gate
		# shellcheck disable=SC2086
		( cd "$scratch" && TMPDIR="$scratch" \
		    SOAK_WRAP="uringscope -f --json=$j --" ${cmd#@wrap } \
		    > "$out/$name.out" 2>&1 ) || {
			printf '%-14s %-9s (the soak itself failed; see %s)\n' \
			    "$name" FAIL "$out/$name.out"
			echo 1 > "$out/.fail"
		}
		;;
	*)
		# shellcheck disable=SC2086
		( cd "$scratch" && TMPDIR="$scratch" uringscope -f --json="$j" \
		    -- $cmd > "$out/$name.out" 2>&1 ) || true
		;;
	esac
	if [ ! -s "$j" ]; then
		printf '%-14s %-9s (no report; see %s)\n' "$name" ERROR "$out/$name.out"
		echo 1 > "$out/.fail"
		continue
	fi
	sub=$(jq '.counters.submissions // 0' "$j")
	ent=$(jq '.counters.enter_calls // 0' "$j")
	pun=$(jq '.counters.punted // 0' "$j")
	tags=$(jq -r '[.findings[]? | "\(.severity):\(.tag)"] | unique | join(",")' "$j")
	bad=$(jq -r --arg allow ",$allow," '[.findings[]?
	    | select(.severity == "WARN" or .severity == "CRIT") | .tag as $t
	    | select(($allow | contains("," + $t + ",")) | not) | $t]
	    | unique | join(",")' "$j")
	allowed=$(jq -r --arg allow ",$allow," '[.findings[]?
	    | select(.severity == "WARN" or .severity == "CRIT") | .tag as $t
	    | select($allow | contains("," + $t + ",")) | $t] | length' "$j")
	# ERRORS only from cancelled polls is not a failure (see above)
	nperr=$(jq '[.ops[]? | select(.op != "POLL_ADD") | .errors] | add // 0' "$j")
	if [ "$nperr" = 0 ]; then
		bad=$(printf '%s' "$bad" | tr ',' '\n' | grep -v '^ERRORS$' | paste -sd, -)
	fi
	if jq -e '[.findings[]? | select(.tag == "NODATA")] | length > 0' "$j" > /dev/null; then
		verdict=NODATA
		echo 1 > "$out/.fail"
	elif [ -n "$bad" ]; then
		verdict=FAIL
		echo 1 > "$out/.fail"
	elif [ "$allowed" -gt 0 ]; then
		verdict=EXPECTED
	else
		verdict=ok
	fi
	ratio=$(awk -v s="$sub" -v e="$ent" 'BEGIN{ if (e > 0) printf "%.2f", s / e; else print "-" }')
	ppct=$(awk -v p="$pun" -v s="$sub" 'BEGIN{ if (s > 0) printf "%.1f", 100 * p / s; else print "-" }')
	printf '%-14s %-9s %10s %10s %8s %7s  %s\n' "$name" "$verdict" "$sub" "$ent" \
	    "$ratio" "$ppct" "${tags:-none}"
	if [ "${CHECK:-0}" = 1 ]; then
		cj="$out/$name.check.json"
		case $cmd in
		"@wrap "*)
			# shellcheck disable=SC2086
			( cd "$scratch" && TMPDIR="$scratch" \
			    SOAK_WRAP="uringscope -f --check --json=$cj --" \
			    ${cmd#@wrap } > "$out/$name.check.out" 2>&1 ) || true
			;;
		*)
			# shellcheck disable=SC2086
			( cd "$scratch" && TMPDIR="$scratch" uringscope -f --check \
			    --json="$cj" -- $cmd > "$out/$name.check.out" 2>&1 ) || true
			;;
		esac
		h=$(jq -r '[.findings[]? | select(.tag | startswith("HAZARD")) | .tag] | unique | join(",")' \
		    "$out/$name.check.json" 2>/dev/null || echo "?")
		if [ -n "$h" ]; then
			printf '%-14s %-9s %s\n' "$name" FAIL "--check: $h"
			echo 1 > "$out/.fail"
		fi
	fi
done
find "$scratch" -mindepth 1 -delete 2>/dev/null || true
if [ -f "$out/.fail" ]; then
	echo "uring-gate: FAIL (reports in $out)"
	exit 1
fi
echo "uring-gate: ok (reports in $out)"
