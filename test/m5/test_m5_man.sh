#!/bin/sh
# test/m5/test_m5_man.sh -- D11
set -eu
: "${XTC_SRC_DIR:?}"

PAGE="$XTC_SRC_DIR/man/man3/xtc_exec.3"
H="$XTC_SRC_DIR/src/inc/xtc_exec.h"

[ -f "$PAGE" ] || { echo "  [D11] FAIL: $PAGE missing" >&2; exit 1; }

# The header minus its "Deprecated names" block: an old name is
# documented on the page of its replacement (docs/deprecations.md).
LIVE=$(sed '/Deprecated names (docs\/deprecations.md)/,/^#ifdef __cplusplus/d' "$H")

fns=$(printf "%s\n" "$LIVE" | grep -oE 'xtc_exec_[a-z_]+' | sort -u)
for fn in $fns; do
	if ! grep -q "$fn" "$PAGE"; then
		echo "  [D11] FAIL: $PAGE missing $fn" >&2
		exit 1
	fi
done

if ! grep -qiE "(RETURN VALUE|RETURNS)" "$PAGE"; then
	echo "  [D11] FAIL: $PAGE has no RETURN VALUE section" >&2
	exit 1
fi

echo "  [D11] OK: xtc_exec.3 covers every M5 function"
