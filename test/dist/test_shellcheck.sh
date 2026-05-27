#!/bin/sh
# test/dist/test_shellcheck.sh -- T4
set -eu
: "${XTC_SRC_DIR:?}"

if ! command -v shellcheck >/dev/null 2>&1; then
	echo "  [T4] SKIP: shellcheck not on PATH"
	exit 0
fi

bad=0
for f in "$XTC_SRC_DIR"/dist/s_*; do
	[ -f "$f" ] || continue
	# Skip non-shell helpers (e.g. .awk).
	case "$f" in *.awk) continue ;; esac
	if ! shellcheck -S warning "$f"; then
		bad=$((bad+1))
	fi
done
[ "$bad" -eq 0 ] || exit 1
echo "  [T4] OK: shellcheck passes on all dist/s_* shell scripts"
