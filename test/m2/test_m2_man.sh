#!/bin/sh
# test/m2/test_m2_man.sh -- D8
# Verify the xtc_io man3 page mentions every public symbol and flag.
set -eu
: "${XTC_SRC_DIR:?}"

PAGE="$XTC_SRC_DIR/man/man3/xtc_io.3"
H="$XTC_SRC_DIR/src/inc/xtc_io.h"

[ -f "$PAGE" ] || { echo "  [D8] FAIL: $PAGE missing" >&2; exit 1; }

# Functions
fns=$(grep -oE 'xtc_io_[a-z_]+' "$H" | sort -u)
for fn in $fns; do
	if ! grep -q "$fn" "$PAGE"; then
		echo "  [D8] FAIL: man page missing function $fn" >&2
		exit 1
	fi
done

# Flag macros
flags=$(grep -oE 'XTC_IO_[A-Z]+' "$H" | sort -u)
for f in $flags; do
	if ! grep -q "$f" "$PAGE"; then
		echo "  [D8] FAIL: man page missing flag $f" >&2
		exit 1
	fi
done

if ! grep -qiE "(RETURN VALUE|RETURNS)" "$PAGE"; then
	echo "  [D8] FAIL: $PAGE has no RETURN VALUE section" >&2
	exit 1
fi

echo "  [D8] OK: xtc_io.3 mentions every function and flag from xtc_io.h"
