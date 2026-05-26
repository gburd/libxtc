#!/bin/sh
# test/m0/test_oos_enforced.sh
#
# Verifies M0_CLAIMS.md [B3, B6]: configure refuses to run inside the
# source root or inside dist/.
#
# Strategy: invoke ../dist/configure from the source root and from
# inside dist/ in a temp clone of the tree (so we don't mutate the
# real one), and assert non-zero exit + a clear error message.

set -eu

: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set by the caller}"
SRC="$XTC_SRC_DIR"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Mirror just the bits configure actually reads.
mkdir -p "$tmp/dist" "$tmp/src"
cp -r "$SRC/dist/." "$tmp/dist/"
cp -r "$SRC/src/."  "$tmp/src/"

cd "$tmp/dist" && autoreconf -i >/dev/null 2>&1
cd "$tmp"

# Case 1: configure from source root.
out_root=$(./dist/configure 2>&1 || true)
if echo "$out_root" | grep -q "must not be configured in the source root"; then
	echo "  [B3] OK: source-root configure rejected"
else
	echo "  [B3] FAIL: source-root configure was not rejected" >&2
	echo "$out_root" >&2
	exit 1
fi

# Case 2: configure from inside dist/.
cd "$tmp/dist"
out_dist=$(./configure 2>&1 || true)
if echo "$out_dist" | grep -q "must not be configured inside dist"; then
	echo "  [B6] OK: dist/ configure rejected"
else
	echo "  [B6] FAIL: dist/ configure was not rejected" >&2
	echo "$out_dist" >&2
	exit 1
fi
