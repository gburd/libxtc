#!/bin/sh
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# scripts/perf-gate.sh -- the release-over-release performance gate.
#
# Builds the PREVIOUS release (a git tag) and the CANDIDATE (a commit) from
# source on THIS host, runs the guarded cells for both, interleaved, REPS
# times each, and fails if any cell regressed.  Nothing is compared against
# a stored number: a stored baseline goes stale the moment the host, the
# kernel, the compiler or the idle-state settings change, and those changes
# moved every number on a 64-vCPU host by up to 5x during 1.59's
# qualification.  A live A/B on one host cancels all of that.
#
# A cell REGRESSES when BOTH hold:
#   1. the candidate's median is worse than the base's by more than its
#      threshold (THRESH, default 5%; noisy cells carry their own), and
#   2. the two runs' ranges do not overlap (every candidate rep is worse
#      than every base rep).
# Rule 2 keeps one-sided noise from failing the gate; rule 1 keeps a tiny
# but consistent shift from failing it.  With REPS=5 an unchanged build
# fails rule 2 for a given cell with probability 2/252 (~0.8%) by chance.
#
# Before measuring, the host must be measurement-grade (see preflight):
# deep CPU idle states off, the performance governor where cpufreq exists,
# and an otherwise idle machine.  A host that is not is REFUSED (exit 3),
# because its results do not mean anything; PERF_GATE_FORCE=1 runs anyway
# and stamps every result line "unqualified".
#
# usage: scripts/perf-gate.sh BASE_REF CAND_REF [OUTDIR]
#   e.g. scripts/perf-gate.sh v1.58.0 HEAD /tmp/perf-1.59
# env:   REPS (default 5)  THRESH (default 0.05)  JOBS (default nproc)
#        CELLS="name ..." to run a subset; PERF_GATE_FORCE=1 (see above)
#        PERF_GATE_ACCEPT="name ..." regressions decided to ship: printed as
#        ACCEPTED, not failing; name each in the release notes
# exit:  0 no regression, 1 a regression, 2 setup/build error,
#        3 host not measurement-grade
#
# Output: OUTDIR/results.csv (one row per rep), OUTDIR/summary.txt (the
# verdict table) and OUTDIR/host.txt (the host record, for the release
# notes: CPU model, cores, kernel, compiler, idle states, governor).
# Keep OUTDIR outside the repository: results are not committed.

set -eu

base_ref=${1:?usage: perf-gate.sh BASE_REF CAND_REF [OUTDIR]}
cand_ref=${2:?usage: perf-gate.sh BASE_REF CAND_REF [OUTDIR]}
out=${3:-${TMPDIR:-/tmp}/perf-gate.$$}
REPS=${REPS:-5}
THRESH=${THRESH:-0.05}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

# shellcheck disable=SC1007
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$out"
# shellcheck disable=SC1007
out=$(CDPATH= cd -- "$out" && pwd)

# ---- the guarded cells ------------------------------------------------
# name | higher_is_better(1/0) | threshold | command (run in the build dir;
# @SX@ = the sqlxtc example dir, @TMP@ = a scratch dir) | awk program that
# prints the one number to compare from the command's stdout.
# Chosen to cover libxtc's critical paths: the fiber switch and same-loop
# messaging (bench_micro), cross-loop messaging (bench_hop), off-loop
# locks at low and high thread counts (bench_sync_offloop: the 1.59 lock
# work moved these 2-12x and its first version regressed at 4 threads),
# timers at scale, aio submit/complete, and the WAL group commit.
cells() {
	cat <<'EOF'
micro_fiber_switch|0|0.10|@MICRO@|-F, '$1=="fiber_switch"{print $2}'
micro_proc_send_recv|0|0.10|@MICRO@|-F, '$1=="proc_send_recv"{print $2}'
micro_timer_set_fire|0|0.10|@MICRO@|-F, '$1=="timer_set_fire"{print $2}'
micro_slab_alloc_free|0|0.10|@MICRO@|-F, '$1=="slab_alloc_free"{print $2}'
micro_amutex_lock|0|0.10|@MICRO@|-F, '$1=="amutex_lock"{print $2}'
micro_chan_mpsc|0|0.10|@MICRO@|-F, '$1=="chan_mpsc"{print $2}'
micro_deque_steal|0|0.10|@MICRO@|-F, '$1=="deque_steal"{print $2}'
micro_chash_get|0|0.10|@MICRO@|-F, '$1=="chash_get"{print $2}'
hop_same_2x64|1|0.05|./bench_hop 2 64 20000 1|'{for(i=1;i<=NF;i++)if($i~/^hops_per_s=/){sub(/.*=/,"",$i);print $i}}'
hop_cross_2x64|1|0.05|./bench_hop 2 64 20000 0|'{for(i=1;i<=NF;i++)if($i~/^hops_per_s=/){sub(/.*=/,"",$i);print $i}}'
hop_cross_16x64|1|0.05|./bench_hop 16 64 5000 0|'{for(i=1;i<=NF;i++)if($i~/^hops_per_s=/){sub(/.*=/,"",$i);print $i}}'
lock_amutex_w4|1|0.05|./bench_sync_offloop amutex w 4 3 20|-F, '{print $6}'
lock_amutex_w64|1|0.05|./bench_sync_offloop amutex w 64 3 20|-F, '{print $6}'
lock_arwlock_mix16|1|0.05|./bench_sync_offloop arwlock mix 16 3 20|-F, '{print $6}'
lock_arwlock_r64|1|0.05|./bench_sync_offloop arwlock r 64 3 20|-F, '{print $6}'
timer_scale_8|1|0.05|./bench_timer_scale 8 2000 512|'{for(i=1;i<=NF;i++)if($i=="timers/sec"){print $(i-1)}}'
exec_spawn_8|1|0.05|./bench_exec_scale 8 4000000 spawn|'{for(i=1;i<=NF;i++)if($i=="tasks/sec"){print $(i-1)}}'
wal_fiber_c8|1|0.05|@SX@/bench_wal fiber 8 256 3 2|-F, '{print $4}'
wal_thread_c1|1|0.05|@SX@/bench_wal thread 1 256 3|-F, '{print $4}'
EOF
}

# ---- preflight: is this host measurement-grade? -------------------------
preflight() {
	bad=""
	{
		echo "date: $(date -u '+%Y-%m-%d %H:%M UTC')"
		echo "kernel: $(uname -srm)"
		echo "cpu: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //')"
		echo "cpus: $(nproc 2>/dev/null)"
		echo "cc: $(${CC:-cc} --version 2>/dev/null | head -1)"
	} > "$out/host.txt"
	# Deep idle states add 80-130 us to every wake that lands on an idle
	# CPU and dominated the 1.59 matrix; state0/1 (POLL, C1) are fine.
	for f in /sys/devices/system/cpu/cpu*/cpuidle/state[2-9]/disable; do
		[ -r "$f" ] || continue
		if [ "$(cat "$f")" != 1 ]; then
			bad="$bad deep-idle-states-enabled"
			break
		fi
	done
	echo "idle: $(cat /sys/devices/system/cpu/cpuidle/current_driver 2>/dev/null || echo none)," \
	    "deep states $( echo "$bad" | grep -q deep-idle && echo ENABLED || echo off)" >> "$out/host.txt"
	g=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
	if [ -r "$g" ]; then
		echo "governor: $(cat $g)" >> "$out/host.txt"
		[ "$(cat $g)" = performance ] || bad="$bad governor-$(cat $g)"
	else
		echo "governor: none (no cpufreq)" >> "$out/host.txt"
	fi
	# An idle machine: the 1-minute load must be below 1 before we start.
	load=$(cut -d' ' -f1 /proc/loadavg 2>/dev/null || echo 0)
	echo "load1 at start: $load" >> "$out/host.txt"
	if awk -v l="$load" 'BEGIN{exit !(l >= 1.0)}'; then
		bad="$bad busy-host(load=$load)"
	fi
	if [ -n "$bad" ]; then
		echo "perf-gate: host is not measurement-grade:$bad" >&2
		echo "perf-gate: disable deep idle states (echo 1 > .../cpuidle/stateN/disable" \
		    "for N >= 2), set the performance governor, stop other jobs" >&2
		if [ "${PERF_GATE_FORCE:-0}" != 1 ]; then
			exit 3
		fi
		echo "unqualified:$bad" >> "$out/host.txt"
		QUAL=unqualified
	fi
}

# ---- build one ref ------------------------------------------------------
build() {
	tag=$1 ref=$2
	t="$out/$tag"
	rm -rf "$t"
	mkdir -p "$t/src" "$t/build"
	if ! git -C "$src" archive "$ref" | tar x -C "$t/src"; then
		echo "perf-gate: cannot archive $ref" >&2
		exit 2
	fi
	( cd "$t/build" && "$t/src/dist/configure" --with-tls=auto \
	    > configure.log 2>&1 && "${MAKE:-make}" -j"$JOBS" > make.log 2>&1 ) || {
		echo "perf-gate: $tag ($ref) failed to build; see $t/build" >&2
		exit 2
	}
	# Both sides build the CANDIDATE's bench sources against their own
	# library: a bench the base release predates still measures the
	# base's library (benches use only the public xtc_* API), and a
	# bench whose source changed cannot fake a difference.  Compiled here,
	# not through the ref's Makefile, which may predate the bench.
	libs=$(sed -n 's/^LIBS[[:space:]]*=//p' "$t/build/Makefile")
	cflags="-O2 -g -std=c11 -D_DEFAULT_SOURCE -I$t/build -I$t/src/src/inc"
	for f in micro/bench_micro bench_hop bench_sync_offloop bench_timer_scale \
	    bench_exec_scale; do
		[ -f "$out/cand-bench/$f.c" ] || continue
		# shellcheck disable=SC2086
		${CC:-cc} $cflags -o "$t/build/${f##*/}" "$out/cand-bench/$f.c" \
		    "$t/build/libxtc.a" -pthread -lm $libs >> "$t/build/make.log" 2>&1 || \
		    echo "perf-gate: $tag: ${f##*/} did not build" >&2
	done
	sx="$t/src/examples/06_sqlxtc"
	if [ -f "$out/cand-bench/bench_wal.c" ]; then
		cp "$out/cand-bench/bench_wal.c" "$sx/bench_wal.c"
		# bench_wal links the ref's own wal.c, the engine under test; a
		# wal.c too old for the bench's calls fails here and the cell is
		# skipped for that side (reported, not judged).
		# shellcheck disable=SC2086
		( cd "$sx" && ${CC:-cc} -O2 -g -std=c11 -D_GNU_SOURCE -I. \
		    -I"$t/src/src/inc" -I"$t/build" -o bench_wal bench_wal.c wal.c \
		    "$t/build/libxtc.a" -pthread -lm $libs \
		    >> "$t/build/make.log" 2>&1 ) || \
		    echo "perf-gate: $tag: bench_wal did not build" >&2
	fi
	git -C "$src" rev-parse --short "$ref" > "$t/rev"
}

# ---- run one cell once --------------------------------------------------
run_cell() {
	tag=$1 name=$2 cmd=$3 prog=$4
	t="$out/$tag"
	sx="$t/src/examples/06_sqlxtc"
	scratch="$out/tmp.$tag"
	mkdir -p "$scratch"
	if [ "$cmd" = @MICRO@ ]; then
		# One bench_micro run per side per rep feeds every micro cell
		# (the suite takes ~3 minutes: running it per cell would not fit).
		[ -s "$t/micro.$rep.csv" ] || return 1
		v=$(eval awk "$prog" "$t/micro.$rep.csv" | tail -1)
		[ -n "$v" ] || return 1
		echo "$v"
		return 0
	fi
	c=$(printf '%s' "$cmd" | sed "s|@SX@|$sx|g; s|@TMP@|$scratch|g")
	exe=$(printf '%s' "$c" | cut -d' ' -f1)
	[ -x "$t/build/$exe" ] || [ -x "$exe" ] || return 1
	# The WAL bench writes its log in the current directory.
	v=$( cd "$t/build" && TMPDIR="$scratch" eval "$c" 2>/dev/null | \
	    eval awk "$prog" | tail -1 )
	find "$scratch" -mindepth 1 -delete 2>/dev/null || true
	[ -n "$v" ] || return 1
	echo "$v"
}

QUAL=qualified
preflight
echo "perf-gate: base $base_ref, candidate $cand_ref, REPS=$REPS -> $out"
# The candidate's bench sources, shared by both sides (see build()).
mkdir -p "$out/cand-bench/micro"
for f in micro/bench_micro bench_hop bench_sync_offloop bench_timer_scale \
    bench_exec_scale; do
	git -C "$src" show "$cand_ref:bench/$f.c" > "$out/cand-bench/$f.c" 2>/dev/null || \
	    rm -f "$out/cand-bench/$f.c"
done
git -C "$src" show "$cand_ref:examples/06_sqlxtc/bench_wal.c" \
    > "$out/cand-bench/bench_wal.c" 2>/dev/null || rm -f "$out/cand-bench/bench_wal.c"
build base "$base_ref"
build cand "$cand_ref"
echo "base $(cat "$out/base/rev") $base_ref" >> "$out/host.txt"
echo "cand $(cat "$out/cand/rev") $cand_ref" >> "$out/host.txt"

echo "cell,rep,side,value,qual" > "$out/results.csv"
rep=1
while [ "$rep" -le "$REPS" ]; do
	# Alternate the order every rep so drift cannot favor a side.
	if [ $((rep % 2)) -eq 1 ]; then order="base cand"; else order="cand base"; fi
	for side in $order; do
		if [ -x "$out/$side/build/bench_micro" ]; then
			( cd "$out/$side/build" && ./bench_micro 1 > "micro.$rep.csv" 2>/dev/null ) || true
			mv "$out/$side/build/micro.$rep.csv" "$out/$side/micro.$rep.csv" 2>/dev/null || true
		fi
	done
	cells | while IFS='|' read -r name _hib _th cmd prog; do
		case " ${CELLS:-$name} " in *" $name "*) ;; *) continue ;; esac
		for side in $order; do
			if v=$(run_cell "$side" "$name" "$cmd" "$prog"); then
				echo "$name,$rep,$side,$v,$QUAL" >> "$out/results.csv"
			fi
		done
	done
	echo "  rep $rep of $REPS done"
	rep=$((rep + 1))
done

python3 "$src/scripts/perf-gate-judge.py" "$out/results.csv" "$THRESH" \
    "$(cells | cut -d'|' -f1-3 | tr '\n' ' ')" > "$out/summary.txt"
rc=$?
cat "$out/summary.txt"
exit $rc
