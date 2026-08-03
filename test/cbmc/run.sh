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
# Usage: ./run.sh [harness-name ...]   (default: all)
#        CBMC_TIMEOUT=1200 ./run.sh    (per-harness wall budget, sec)

set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
INC="$DIR/../../src/inc"
TIMEOUT="${CBMC_TIMEOUT:-1200}"

if ! command -v cbmc >/dev/null 2>&1; then
	echo "  [cbmc] SKIP: cbmc not on PATH (nix-shell -p cbmc)"
	exit 0
fi

# Per-harness CBMC flags.  Lock-free / SC-fence-heavy models get a
# small unwind (the algorithms are bound-agnostic; the interesting
# races appear at 1-2 concurrent actors) and memory-safety checks are
# left OFF where the property is purely the concurrency invariant (a
# separate ASan tier covers memory safety) -- this keeps each check
# tractable.  Tune here, per harness, not globally.
#
# format: <harness-basename> <cbmc-flags...>
HARNESSES='
deque_harness      --unwind 4
lwlock_harness     --unwind 6
lrlock_harness     --unwind 6
rcu_harness        --unwind 6
refcount_harness   --unwind 6
wakepark_harness   --unwind 6
mpsc_harness       --unwind 6
credit_harness     --unwind 8
seqlock_harness    --unwind 6
chash_resize_harness --unwind 6
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
