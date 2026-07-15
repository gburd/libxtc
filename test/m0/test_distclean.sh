#!/bin/sh
# test/m0/test_distclean.sh -- B5
#
# Verifies that `make distclean` removes every file produced by
# configure and make.  Runs in an isolated clone so it doesn't
# destroy the active build dir.

set -eu
: "${XTC_SRC_DIR:?}"

# This gate regenerates the build system with autoreconf; on a host
# without autoconf/automake (e.g. a stock macOS box) there is nothing
# to verify -- skip cleanly rather than abort `make check` under set -e.
if ! command -v autoreconf >/dev/null 2>&1; then
	echo "  [distclean] SKIP: autoreconf not available"
	exit 0
fi

tmp=$(mktemp -d)
trap 'cd / 2>/dev/null; rm -rf "$tmp"' EXIT
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
