#!/bin/sh
# examples/check_help.sh -- every example binary must answer --help.
#
#   usage: check_help.sh BINARY...
#
# For each binary: `BINARY --help` must exit 0 within 2 seconds AND print
# a line containing "usage" (any case) on stdout or stderr.
#
# Why: 08_tnt's echo server parsed argv with atoi, so `tnt_echo --help`
# became "listen on port 7777" and blocked forever -- it stalled an audit
# for 50 minutes.  A server that treats an unknown flag as "start
# serving" is a trap for every person and script that asks it what it
# does.  The "usage" requirement matters too: several demos ignored argv
# entirely and happened to finish in under 2 s, which a timeout-only
# check would have passed.
#
# Needs timeout(1) (coreutils; `gtimeout` on macOS/Homebrew).  Without it
# the check cannot be bounded, so it SKIPS loudly rather than risk the
# very hang it exists to catch.
set -u

TO=
for t in timeout gtimeout; do
	if command -v "$t" >/dev/null 2>&1; then TO=$t; break; fi
done
if [ -z "$TO" ]; then
	echo "  [help] SKIP: no timeout(1)/gtimeout(1); --help lint not run"
	exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/xtc-help.XXXXXX") || exit 1
trap 'rm -f "$tmp"' EXIT

fail=0
n=0
for b in "$@"; do
	n=$((n + 1))
	# -k: if --help ignores SIGTERM too, SIGKILL it 1 s later.
	"$TO" -k 1 2 "$b" --help >"$tmp" 2>&1
	rc=$?
	if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
		echo "  [help] FAIL: $b --help did not exit within 2s (it ran" \
		    "the program instead of printing usage)"
		fail=$((fail + 1))
	elif [ "$rc" -ne 0 ]; then
		echo "  [help] FAIL: $b --help exited $rc (want 0)"
		sed 's/^/      /' "$tmp" | head -5
		fail=$((fail + 1))
	elif ! grep -qi usage "$tmp"; then
		echo "  [help] FAIL: $b --help printed no usage line"
		sed 's/^/      /' "$tmp" | head -5
		fail=$((fail + 1))
	fi
done
if [ "$fail" -ne 0 ]; then
	echo "  [help] FAIL: $fail of $n example binaries mishandle --help"
	exit 1
fi
echo "  [help] OK: all $n example binaries answer --help (exit 0, < 2s, usage)"
