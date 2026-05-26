#!/bin/sh
# test/m0/test_man_coverage.sh — D4
# Every public function declared in xtc.h must have a man3 page.
set -eu
: "${XTC_SRC_DIR:?}"

H="$XTC_SRC_DIR/src/inc/xtc.h"
MANDIR="$XTC_SRC_DIR/man/man3"

# Extract function names: find lines in xtc.h that look like a public
# declaration "<type> xtc_<name>(...);" and pull out xtc_<name>.
funcs=$(awk '
	/^[A-Za-z_].*xtc_[A-Za-z0-9_]+\(/ {
		match($0, /xtc_[A-Za-z0-9_]+/);
		print substr($0, RSTART, RLENGTH);
	}
' "$H" | sort -u)

if [ -z "$funcs" ]; then
	echo "  [D4] FAIL: no functions extracted from $H" >&2
	exit 1
fi

missing=""
for f in $funcs; do
	if [ ! -f "$MANDIR/$f.3" ]; then
		missing="$missing $f"
	fi
done

if [ -n "$missing" ]; then
	echo "  [D4] FAIL: missing man pages for:$missing" >&2
	exit 1
fi
echo "  [D4] OK: $(echo "$funcs" | wc -w) public functions, all documented in man3"
