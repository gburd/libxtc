#!/bin/sh
# test/dist/test_s_perm.sh -- T5
set -eu
: "${XTC_SRC_DIR:?}"
"$XTC_SRC_DIR/dist/s_perm"
bad=0
for f in "$XTC_SRC_DIR"/dist/s_*; do
	[ -f "$f" ] || continue
	if [ ! -x "$f" ]; then
		echo "  [T5] FAIL: $f not executable" >&2
		bad=$((bad+1))
	fi
done
[ "$bad" -eq 0 ] || exit 1
echo "  [T5] OK: every dist/s_* is executable"
