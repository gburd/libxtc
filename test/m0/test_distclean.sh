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
# Copy the source tree, but EXCLUDE sibling build dirs (build_bsd/,
# build_ci/, build_shared/, ...).  test/m0/test_distclean runs inside
# a build dir that lives in the tree, so a naive `cp -r .` recursively
# copies the active build's objects/libs -- slow and pointless (we
# regenerate from dist/).  Copy dist/ + src/ + test/ + the top-level
# files we need; that is all autoreconf/configure/make touch.
mkdir -p "$tmp"
for d in dist src test examples man docs bench; do
	[ -d "$XTC_SRC_DIR/$d" ] && cp -r "$XTC_SRC_DIR/$d" "$tmp/"
done

log="$tmp/step.log"

# Regenerate + build, surfacing the failing step's output (previously
# each step was silenced with >/dev/null 2>&1, so an autoreconf/
# configure/make failure -- e.g. in a resource-constrained VM -- died
# under set -e with NO diagnostic).
cd "$tmp/dist"
if ! autoreconf -i >"$log" 2>&1; then
	echo "  [B5] FAIL: autoreconf errored:" >&2; tail -15 "$log" >&2; exit 1
fi
mkdir -p "$tmp/build"
cd "$tmp/build"
if ! ../dist/configure >"$log" 2>&1; then
	echo "  [B5] FAIL: configure errored:" >&2; tail -15 "$log" >&2; exit 1
fi
if ! ${MAKE:-make} >"$log" 2>&1; then
	echo "  [B5] FAIL: make errored:" >&2; tail -20 "$log" >&2; exit 1
fi

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
