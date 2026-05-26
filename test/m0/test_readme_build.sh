#!/bin/sh
# test/m0/test_readme_build.sh
# Verifies M0_CLAIMS.md [D1]: the build commands in README.md actually work.
#
# Strategy: pull the lines tagged with comments `# M0_CLAIMS:B1` and
# `# M0_CLAIMS:B2` from README.md and run the B1 sequence in a tmp dir.
# The B2 (meson) sequence is run only if meson is on PATH.

set -eu

: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set}"
README="$XTC_SRC_DIR/README.md"

if [ ! -f "$README" ]; then
	echo "  [D1] FAIL: README.md missing" >&2
	exit 1
fi

extract() {
	awk -v tag="$1" '
		$0 ~ "M0_CLAIMS:" tag "_BEGIN" { p=1; next }
		$0 ~ "M0_CLAIMS:" tag "_END"   { p=0 }
		p && $0 !~ /^```/              { print }
	' "$README"
}

# B1.
b1=$(extract B1)
if [ -z "$b1" ]; then
	echo "  [D1] FAIL: README has no B1 build block" >&2
	exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp -r "$XTC_SRC_DIR/." "$tmp/"
cd "$tmp/dist" && autoreconf -i >/dev/null 2>&1
cd "$tmp"

# Run the B1 commands.  We expand $XTC_SRC for documentation clarity
# in the README; in the test harness the cwd is a clone.
( eval "$b1" ) >"$tmp/b1.log" 2>&1 || {
	echo "  [D1] FAIL: B1 commands from README failed" >&2
	cat "$tmp/b1.log" >&2
	exit 1
}
[ -f "$tmp/build_unix/libxtc.a" ] || {
	echo "  [D1] FAIL: B1 did not produce libxtc.a" >&2
	exit 1
}
echo "  [D1] OK: B1 commands from README produce libxtc.a"

# B2 (optional).
if command -v meson >/dev/null 2>&1 && command -v ninja >/dev/null 2>&1; then
	b2=$(extract B2)
	if [ -n "$b2" ]; then
		( eval "$b2" ) >"$tmp/b2.log" 2>&1 || {
			echo "  [D1] FAIL: B2 commands from README failed" >&2
			cat "$tmp/b2.log" >&2
			exit 1
		}
		echo "  [D1] OK: B2 (meson) commands from README work"
	fi
else
	echo "  [D1] SKIP: meson/ninja not present, B2 path not exercised"
fi
