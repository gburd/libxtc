#!/bin/sh
# test/m4/test_m4_man.sh -- D10
set -eu
: "${XTC_SRC_DIR:?}"

PAGE="$XTC_SRC_DIR/man/man3/xtc_async.3"
H="$XTC_SRC_DIR/src/inc/xtc_async.h"

[ -f "$PAGE" ] || { echo "  [D10] FAIL: $PAGE missing" >&2; exit 1; }

# Public functions.
fns=$(grep -oE 'xtc_(async|await|yield|stack_size|set_stack_size)\b' "$H" | sort -u)
for fn in $fns; do
	if ! grep -q "$fn" "$PAGE"; then
		echo "  [D10] FAIL: $PAGE missing $fn" >&2
		exit 1
	fi
done

# Macros.
for k in XTC_COOP_REGION XTC_PT_THREAD XTC_PT_BEGIN XTC_PT_END XTC_PT_YIELD XTC_PT_WAIT_UNTIL; do
	if ! grep -q "$k" "$PAGE"; then
		echo "  [D10] FAIL: $PAGE missing macro $k" >&2
		exit 1
	fi
done

if ! grep -qiE "(RETURN VALUE|RETURNS)" "$PAGE"; then
	echo "  [D10] FAIL: no RETURN VALUE section" >&2
	exit 1
fi

echo "  [D10] OK: xtc_async.3 covers every M4 function and macro"
