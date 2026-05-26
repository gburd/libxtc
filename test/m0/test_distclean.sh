#!/bin/sh
# test/m0/test_distclean.sh — B5
#
# Verifies that `make distclean` removes every file produced by
# configure and make.  Runs in an isolated clone so it doesn't
# destroy the active build dir.

set -eu
: "${XTC_SRC_DIR:?}"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp -r "$XTC_SRC_DIR/." "$tmp/"

cd "$tmp/dist" && autoreconf -i >/dev/null 2>&1
mkdir -p "$tmp/build"
cd "$tmp/build"
../dist/configure >/dev/null 2>&1
make >/dev/null 2>&1

# Snapshot artefacts known to be generated.
for f in Makefile config.log config.status xtc_config.h libxtc.a; do
	if [ ! -e "$f" ]; then
		echo "  [B5] FAIL: pre-distclean: $f not produced" >&2
		exit 1
	fi
done

make distclean >/dev/null

# After distclean every generated file must be gone.
for f in Makefile config.log config.status xtc_config.h libxtc.a; do
	if [ -e "$f" ]; then
		echo "  [B5] FAIL: $f survived distclean" >&2
		exit 1
	fi
done

echo "  [B5] OK: distclean removed every generated file"
