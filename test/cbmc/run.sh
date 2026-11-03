#!/bin/sh
# test/cbmc/run.sh -- run the libxtc bounded-model-checking tier.
#
# Two kinds of harness (see README.md):
#   - test/cbmc/*_harness.c: a concurrent algorithm, modelled.  Where the
#     model copies statements from the source they sit between
#     BEGIN/END SOURCE markers, and check_drift.sh (run first, below, and
#     in make check) fails if the source no longer contains them.
#   - test/cbmc/real/*_harness.c: links or #includes the REAL library
#     source unchanged, with only the allocator/OS calls stubbed, so it
#     cannot drift.  These also run memory-safety checks.
# CBMC explores every interleaving / input up to the harness's bound and
# proves the asserted invariants, or prints a counterexample trace.
#
# Requires cbmc on PATH (nix-shell -p cbmc).  Not in the default
# `make check`; this is a separate verification tier + release gate,
# like check-dst.
#
# CBMC VERSION: use a 5.x release (e.g. 5.95.1).  CBMC 6.x aborts with
# "pointer handling for concurrency is unsound" (exit 6) on the
# concurrent-pointer harnesses (deque); 5.x emits the same note as a
# WARNING and completes to a verdict.  Verified on an idle 16-core box
# with cbmc 5.95.1.
#
# Usage: ./run.sh [harness-name ...]   (default: all)
#        CBMC_TIMEOUT=1800 ./run.sh    (per-harness wall budget, sec)

set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
INC="$DIR/../../src/inc"
TIMEOUT="${CBMC_TIMEOUT:-1800}"

if ! command -v cbmc >/dev/null 2>&1; then
	echo "  [cbmc] SKIP: cbmc not on PATH (nix-shell -p cbmc)"
	exit 0
fi

# CBMC MAJOR VERSION GATE.  6.x aborts with "pointer handling for
# concurrency is unsound" BEFORE producing a verdict on every concurrent
# harness here (verified on 6.4.0 and 6.9.0), so running it would report
# every harness as FAILED for a tool reason -- indistinguishable, to a
# reader or a CI log, from a real invariant violation.  Skip loudly
# instead, naming the version needed.  5.x emits the same note as a
# warning and completes to VERIFICATION SUCCESSFUL, which is what the
# per-harness bounds below were tuned against.
cbmc_ver=$(cbmc --version 2>/dev/null | head -1 | sed 's/[^0-9.].*$//')
cbmc_major=${cbmc_ver%%.*}
case "$cbmc_major" in
[0-9]*)
	if [ "$cbmc_major" -ge 6 ]; then
		echo "  [cbmc] SKIP: found cbmc $cbmc_ver, but the concurrent"
		echo "         harnesses need CBMC 5.x (validated: 5.95.1)."
		echo "         6.x aborts with 'pointer handling for concurrency"
		echo "         is unsound' before reaching a verdict, so its"
		echo "         'failures' would be a tool limitation, not a"
		echo "         libxtc bug.  Install 5.x ahead on PATH to verify."
		exit 0
	fi
	;;
*)
	echo "  [cbmc] WARN: could not parse 'cbmc --version'; attempting the run"
	;;
esac

# Per-harness CBMC flags.  Each bound was tuned on an idle 16-core box
# (cbmc 5.95.1) to reliably reach VERIFICATION SUCCESSFUL within the
# budget; the wall time at that bound is noted.  The algorithms are
# bound-agnostic (the interesting races appear at 1-3 concurrent
# actors), so a small unwind checks the identical logic in a tractable
# state space.  Tune here, per harness, not globally.
#
# format: <harness-basename> <cbmc-flags...>
#
# harness               bound   wall   note
# deque_harness         u4      ~0.3s  real deque.h; Chase-Lev
# lwlock_harness        u4      ~21s   state-word CAS core
# credit_harness        u8      ~55s   sliding-window semaphore
# seqlock_harness       u6      ~0.1s  REAL bm_read_begin/valid
# mpsc_harness          u5      ~113s  index-FIFO model (u4 vacuous)
# lrlock_harness        u6      ~0.2s  epoch + read_idx flip
# rcu_harness           u6      ~0.3s  epoch reclaim + drain
# refcount_harness      u6      ~0.2s  stripe-locked pin
# mask_harness          u4      ~0.2s  A2 mask vs remote kill (no loss)
# dispatch_once_harness u3      ~0.1s  B1 future resolved exactly once
# wakepark_harness      u6      ~0.1s  v1.8.0 wake latch
# chash_resize_harness  u6      ~0.2s  build-then-swap publish
HARNESSES='
deque_harness      --unwind 4
lwlock_harness     --unwind 4
credit_harness     --unwind 8
seqlock_harness    --unwind 6
mpsc_harness       --unwind 5
lrlock_harness     --unwind 6
rcu_harness        --unwind 6
refcount_harness   --unwind 6
mask_harness       --unwind 4
dispatch_once_harness --unwind 3
wakepark_harness   --unwind 6
chash_resize_harness --unwind 6
res_harness        --unwind 3
hlc_harness        --unwind 4
fdclaim_harness    --unwind 3
wakeclaim_harness  --unwind 3
inbox_harness      --unwind 4
parktimer_harness  --unwind 3
timeout_harness    --unwind 3
combinator_harness --unwind 3
'

# Checks every concurrent harness also runs.  --pointer-check cannot be
# combined with threads in CBMC 5.x ("pointer handling for concurrency
# is unsound"), so it is on the sequential real-code harnesses only.
SAFE="--bounds-check --div-by-zero-check --signed-overflow-check --pointer-overflow-check"

# Real-code harnesses: <name> <sources...> -- <cbmc flags...>.  Sources
# are relative to the repository root and linked into the harness with
# goto-cc; the harness's own stubs replace the allocator and OS calls.
# @RECV marks a harness whose recv/poll are replaced after linking
# (CBMC's built-in models of those win over a definition in the harness).
REAL='
down_decode_harness  src/ptc/proc.c -- --unwind 48 --bounds-check --pointer-check --signed-overflow-check
net_frame_harness    src/io/io_net.c src/os/os_alloc.c src/os/os_time.c @RECV -- --unwind 2 --unwindset xtc_net_recv_frame.0:8,xtc_net_recv_frame.1:7,main.0:8,main.1:5,peer_recv.0:5,__net_wait_fd.0:2 --unwinding-assertions --bounds-check --pointer-check
res_harness          src/ptc/res.c -- --unwind 3 --unwindset xtc_res_init.0:7 --unwinding-assertions --bounds-check --signed-overflow-check
lockmgr_harness      -- --unwind 3 --unwindset __do_acquire_locked.0:1,__do_acquire_locked.1:1,__do_acquire_locked.2:1,__do_acquire_locked.3:1,__do_acquire_locked.4:1,__do_acquire_locked.5:1,__do_acquire_locked.6:1,__do_acquire_locked.7:1,__dominates.0:10,spec_dominates.0:10,main.0:4,xtc_lockmgr_create.0:2,__hash.0:2,memcmp.0:2,holds_at_least.0:5,newest_mode.0:5,exclusive.0:5,exclusive.1:5,__find_granted.0:3,__has_conflict_granted.0:3 --bounds-check --slice-formula
'

want="${*:-}"
rcfile=$(mktemp)
echo 0 > "$rcfile"

# The drift gate first: a transcribed harness that no longer matches its
# source proves nothing about the code that ships.
if ! sh "$DIR/check_drift.sh"; then
	echo 1 > "$rcfile"
fi
printf '%s\n' "$HARNESSES" | while read -r name flags; do
	[ -n "$name" ] || continue
	[ -f "$DIR/$name.c" ] || continue
	if [ -n "$want" ]; then
		case " $want " in *" $name "*) : ;; *) continue ;; esac
	fi
	printf '  [cbmc] %-22s ' "$name"
	# shellcheck disable=SC2086
	if out=$(timeout "$TIMEOUT" cbmc "$DIR/$name.c" -I "$INC" $flags $SAFE 2>&1); then
		if printf '%s' "$out" | grep -q 'VERIFICATION SUCCESSFUL'; then
			echo "OK (verified)"
		else
			echo "UNKNOWN (no verdict -- see below)"
			printf '%s\n' "$out" | tail -5
			echo 1 > "$rcfile"
		fi
	else
		st=$?
		if [ "$st" = 124 ]; then
			echo "TIMEOUT (${TIMEOUT}s) -- raise CBMC_TIMEOUT or lower the bound"
		else
			echo "FAILED (verification counterexample or error)"
			printf '%s\n' "$out" | grep -iE 'FAILURE|assert|VERIFICATION' | head -8
		fi
		echo 1 > "$rcfile"
	fi
done

TOP=$(cd "$DIR/../.." && pwd)
GB=$(mktemp -d "${TMPDIR:-/var/tmp}/cbmc-real.XXXXXX")
printf '%s\n' "$REAL" | while read -r line; do
	[ -n "$line" ] || continue
	name=${line%% *}
	rest=${line#"$name"}
	srcs=${rest%% -- *}
	flags=${rest#* -- }
	[ -f "$DIR/real/$name.c" ] || continue
	if [ -n "$want" ]; then
		case " $want " in *" real/$name "*|*" $name "*) : ;; *) continue ;; esac
	fi
	printf '  [cbmc] %-22s ' "real/$name"
	recv=0
	files=""
	for f in $srcs; do
		case "$f" in
		@RECV) recv=1 ;;
		*) files="$files $TOP/$f" ;;
		esac
	done
	# shellcheck disable=SC2086
	if ! out=$(goto-cc -std=c11 -D_DEFAULT_SOURCE -pthread \
	    -I "$DIR/real/inc" -I "$INC" -I "$TOP/src/ptc" \
	    "$DIR/real/$name.c" $files -o "$GB/$name.0.gb" 2>&1); then
		echo "FAILED (goto-cc)"
		printf '%s\n' "$out" | grep -iE 'error' | head -5
		echo 1 > "$rcfile"
		continue
	fi
	if [ "$recv" = 1 ]; then
		goto-instrument --replace-calls recv:peer_recv \
		    --replace-calls poll:peer_poll \
		    "$GB/$name.0.gb" "$GB/$name.gb" >/dev/null 2>&1
	else
		mv "$GB/$name.0.gb" "$GB/$name.gb"
	fi
	# The log of a long unwind is large: keep it in a file, not a shell
	# variable (a variable that size overflows the argument limit).
	st=0
	# shellcheck disable=SC2086
	timeout "$TIMEOUT" cbmc "$GB/$name.gb" $flags > "$GB/$name.log" 2>&1 ||
	    st=$?
	if [ "$st" = 0 ]; then
		if grep -q 'VERIFICATION SUCCESSFUL' "$GB/$name.log"; then
			echo "OK (verified)"
		else
			echo "UNKNOWN (no verdict -- see below)"
			tail -5 "$GB/$name.log"
			echo 1 > "$rcfile"
		fi
	else
		if [ "$st" = 124 ]; then
			echo "TIMEOUT (${TIMEOUT}s) -- raise CBMC_TIMEOUT or lower the bound"
		else
			echo "FAILED (verification counterexample or error)"
			grep -iE ': FAILURE|VERIFICATION|out of memory' \
			    "$GB/$name.log" | head -8
		fi
		echo 1 > "$rcfile"
	fi
done
find "$GB" -type f \( -name '*.gb' -o -name '*.log' \) -exec rm -f {} +
rmdir "$GB" 2>/dev/null || :

rc=$(cat "$rcfile")
rm -f "$rcfile"
exit "$rc"
