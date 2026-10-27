#!/bin/sh
# scripts/abi-check.sh -- release-time ABI diff of libxtc.so against a tag.
#
# usage: scripts/abi-check.sh BASE_TAG [CAND_REF] [OUTDIR]
#
# Builds the shared library (--enable-shared, -g) at BASE_TAG and at
# CAND_REF (default HEAD) from `git archive`, then runs libabigail's
# abidiff over the two, restricted to the installed public headers
# (--headers-dir), so internal types never count.  Not part of
# `make check`: it needs the old build.  Run it when cutting a release;
# docs/abi-stability.md says when its verdict blocks one.
#
# abidiff exit status is a bit field: 0 = no change, bit 2 (4) = ABI
# CHANGE (additive: a new symbol, an appended field the size-first rules
# allow), bit 3 (8) = INCOMPATIBLE change.  This script exits with that
# status, prints the report path, and fails (nonzero) on bit 1 (usage /
# tool error) or bit 3.  A MINOR release must show no bit 3 against the
# previous release of the same major; a MAJOR is expected to.
#
# abidiff/abidw come from libabigail (Debian: abigail-tools; nix:
# nix-shell -p libabigail).  Nothing here references a host or a
# particular machine; any Linux box with a C toolchain will do.

set -eu

if [ $# -lt 1 ]; then
	echo "usage: $0 BASE_TAG [CAND_REF] [OUTDIR]" >&2
	exit 2
fi
base=$1
cand=${2:-HEAD}
out=${3:-$(mktemp -d "${TMPDIR:-/tmp}/abicheck.XXXXXX")}
src=$(git rev-parse --show-toplevel)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}

if ! command -v abidiff >/dev/null 2>&1; then
	echo "abi-check: abidiff not found (install libabigail)" >&2
	exit 2
fi
mkdir -p "$out"

build() {	# build REF DIR -> DIR/build/libxtc.so.* and DIR/src/src/inc
	ref=$1 t=$2
	mkdir -p "$t/src" "$t/build"
	if ! git -C "$src" archive "$ref" | tar x -C "$t/src"; then
		echo "abi-check: cannot archive $ref" >&2
		return 1
	fi
	( cd "$t/build" && CFLAGS='-g -Og' "$t/src/dist/configure" \
	    --enable-shared --with-tls=auto > configure.log 2>&1 &&
	  make -j"$JOBS" > make.log 2>&1 ) || {
		echo "abi-check: build of $ref failed (see $t/build)" >&2
		return 1
	}
}

build "$base" "$out/base"
build "$cand" "$out/cand"
so_b=$(find "$out/base/build" -maxdepth 1 -name 'libxtc.so.*.*' | head -1)
so_c=$(find "$out/cand/build" -maxdepth 1 -name 'libxtc.so.*.*' | head -1)
if [ -z "$so_b" ] || [ -z "$so_c" ]; then
	echo "abi-check: no shared library built" >&2
	exit 2
fi

set +e
abidiff --headers-dir1 "$out/base/src/src/inc" \
	--headers-dir2 "$out/cand/src/src/inc" \
	--drop-private-types \
	"$so_b" "$so_c" > "$out/abidiff.txt" 2>&1
rc=$?
set -e

echo "abi-check: $base -> $cand: abidiff status $rc (report: $out/abidiff.txt)"
grep -E '^Functions changes summary|^Variables changes summary' \
	"$out/abidiff.txt" || true
echo "abi-check: SONAME $(readelf -d "$so_b" | sed -n 's/.*SONAME.*\[\(.*\)\]/\1/p')" \
	"-> $(readelf -d "$so_c" | sed -n 's/.*SONAME.*\[\(.*\)\]/\1/p')"
if [ $((rc & 2)) -ne 0 ]; then
	echo "abi-check: abidiff usage/tool error" >&2
	exit "$rc"
fi
if [ $((rc & 8)) -ne 0 ]; then
	echo "abi-check: INCOMPATIBLE ABI change (allowed only in a MAJOR)"
fi
exit "$rc"
