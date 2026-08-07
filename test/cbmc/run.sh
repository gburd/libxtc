#!/bin/sh
# test/cbmc/run.sh -- run the libxtc bounded-model-checking tier.
#
# Each harness verifies one hard-to-keep-correct concurrent algorithm
# against its REAL source (see README.md).  CBMC explores every
# interleaving up to the harness's bound and proves the asserted safety
# invariant, or prints a counterexample trace.
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
wakepark_harness   --unwind 6
chash_resize_harness --unwind 6
res_harness        --unwind 3
hlc_harness        --unwind 4
'

want="${*:-}"
rcfile=$(mktemp)
echo 0 > "$rcfile"
printf '%s\n' "$HARNESSES" | while read -r name flags; do
	[ -n "$name" ] || continue
	[ -f "$DIR/$name.c" ] || continue
	if [ -n "$want" ]; then
		case " $want " in *" $name "*) : ;; *) continue ;; esac
	fi
	printf '  [cbmc] %-22s ' "$name"
	# shellcheck disable=SC2086
	if out=$(timeout "$TIMEOUT" cbmc "$DIR/$name.c" -I "$INC" $flags 2>&1); then
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

rc=$(cat "$rcfile")
rm -f "$rcfile"
exit "$rc"
