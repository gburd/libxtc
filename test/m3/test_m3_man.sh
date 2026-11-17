#!/bin/sh
# test/m3/test_m3_man.sh -- D9
# Verify the L2 event-loop man page exists and covers every public symbol.
set -eu
: "${XTC_SRC_DIR:?}"

PAGE="$XTC_SRC_DIR/man/man3/xtc_loop.3"
H="$XTC_SRC_DIR/src/inc/xtc_loop.h"

[ -f "$PAGE" ] || { echo "  [D9] FAIL: $PAGE missing" >&2; exit 1; }

# The header minus its "Deprecated names" block: an old name is
# documented on the page of its replacement (docs/deprecations.md).
LIVE=$(sed '/Deprecated names (docs\/deprecations.md)/,/^#ifdef __cplusplus/d' "$H")

# Public functions: anything xtc_(loop|task|waker|timer)_*.
fns=$(printf "%s\n" "$LIVE" | grep -oE 'xtc_(loop|task|waker|timer)_[a-z_]+' | sort -u)
for fn in $fns; do
	if ! grep -q "$fn" "$PAGE"; then
		echo "  [D9] FAIL: $PAGE missing $fn" >&2
		exit 1
	fi
done

# State constants.
for k in XTC_TASK_DONE XTC_TASK_RESCHED XTC_TASK_PENDING; do
	if ! grep -q "$k" "$PAGE"; then
		echo "  [D9] FAIL: $PAGE missing constant $k" >&2
		exit 1
	fi
done

if ! grep -qiE "(RETURN VALUE|RETURNS)" "$PAGE"; then
	echo "  [D9] FAIL: $PAGE has no RETURN VALUE section" >&2
	exit 1
fi

echo "  [D9] OK: xtc_loop.3 covers every M3 function and constant"
