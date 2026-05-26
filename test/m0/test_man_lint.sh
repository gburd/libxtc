#!/bin/sh
# test/m0/test_man_lint.sh — D5
# Lint every man page with mandoc.  Skipped if mandoc is missing.
set -eu
: "${XTC_SRC_DIR:?}"
if ! command -v mandoc >/dev/null 2>&1; then
	echo "  [D5] SKIP: mandoc not installed"
	exit 0
fi

bad=0
for m in "$XTC_SRC_DIR"/man/man3/*.3 "$XTC_SRC_DIR"/man/man7/*.7; do
	[ -f "$m" ] || continue
	# Check only for ERROR-level lint issues; WARNINGs vary across
	# mandoc versions (FreeBSD's mandoc is stricter than Linux's)
	# and don't represent real defects — "unusual Xr order" and
	# "new sentence, new line" are stylistic preferences.
	if ! out=$(mandoc -T lint -W error "$m" 2>&1); then
		echo "  [D5] FAIL: $m" >&2
		echo "$out" >&2
		bad=$((bad+1))
	elif printf '%s' "$out" | grep -q ': ERROR:'; then
		echo "  [D5] FAIL: $m: $out" >&2
		bad=$((bad+1))
	fi
done
[ "$bad" -eq 0 ] || exit 1
echo "  [D5] OK: every man page passes mandoc lint"
