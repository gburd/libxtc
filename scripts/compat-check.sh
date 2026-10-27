#!/bin/sh
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# scripts/compat-check.sh [BASE_TAG] -- the N-1 consumer compat gate.
#
# A consumer compiled against the PREVIOUS release's installed headers
# must run correctly against THIS tree's library.  The consumer is
# test/abi/test_abi_sized.c TAKEN FROM BASE_TAG (the struct prefixes it
# simulates are the ones that release shipped), built against BASE_TAG's
# installed headers, and linked against HEAD's library:
#
#   1. build + install BASE_TAG (headers only are used) into a prefix;
#   2. build HEAD's libxtc.a (and libxtc.so with --enable-shared);
#   3. compile BASE_TAG's consumer with -I<BASE prefix>/include and link
#      it to HEAD's library; run it.  Static first, then the same object
#      against HEAD's libxtc.so (the dynamic-link swap a packager does).
#
# BASE_TAG defaults to the newest v2.* tag that is not HEAD's own
# version.  Until a 2.x tag exists there is nothing to be compatible
# WITH: 1.x -> 2.0 is the major break (structs gained a size field, the
# symbols a version node), so the job SKIPS (exit 0, says why).  The
# lead wires the first real base once v2.0.0 is tagged; nothing here
# changes then.
#
# Exit 0 pass or skip, 1 the consumer failed against HEAD, 2 setup error.
# env: CC (default cc), JOBS, COMPAT_OUT (work dir, default a temp dir),
#      COMPAT_FORCE=1 runs even against a 1.x base (expected to FAIL; for
#      showing the gate bites).
set -eu

# shellcheck disable=SC1007
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$src"
CC=${CC:-cc}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
head_ver=$(cat dist/version.in)

base=${1:-}
if [ -z "$base" ]; then
	base=$(git tag -l 'v2.*' --sort=-v:refname 2>/dev/null |
	    grep -vx "v$head_ver" | head -1 || true)
fi
if [ -z "$base" ]; then
	echo "compat-check: SKIP: no 2.x release tag yet (HEAD is $head_ver)."
	echo "  The N-1 gate compares against the previous release of the SAME"
	echo "  major; 1.x -> 2.0 is the major break and is not expected to pass."
	echo "  It runs once v2.0.0 is tagged."
	exit 0
fi
if ! git rev-parse --verify -q "$base^{commit}" >/dev/null; then
	echo "compat-check: no such ref $base (fetch tags: git fetch --tags)" >&2
	exit 2
fi
case "$base" in
v2.*|v3.*) ;;
*)
	if [ "${COMPAT_FORCE:-0}" != 1 ]; then
		echo "compat-check: SKIP: $base is not a 2.x release; a 1.x consumer is not"
		echo "  expected to run against 2.x (COMPAT_FORCE=1 to run it anyway)."
		exit 0
	fi
	;;
esac
consumer=test/abi/test_abi_sized.c
if ! git cat-file -e "$base:$consumer" 2>/dev/null; then
	echo "compat-check: $base has no $consumer; nothing to check" >&2
	exit 2
fi

out=${COMPAT_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/xtc-compat.XXXXXX")}
mkdir -p "$out/base-src" "$out/base-build" "$out/base-prefix" "$out/head-build"
echo "compat-check: $base consumer + headers vs HEAD ($head_ver) library, in $out"

git archive "$base" | tar x -C "$out/base-src"
( cd "$out/base-src/dist" && { [ -x configure ] || autoreconf -i >/dev/null 2>&1; } )
( cd "$out/base-build" &&
    CC="$CC" "$out/base-src/dist/configure" --prefix="$out/base-prefix" \
        --with-tls=auto > configure.log 2>&1 &&
    make -j"$JOBS" > make.log 2>&1 && make install > install.log 2>&1 ) || {
	echo "compat-check: building/installing $base failed; see $out/base-build" >&2
	exit 2
}
( cd "$out/head-build" &&
    CC="$CC" "$src/dist/configure" --enable-shared --with-tls=auto \
        > configure.log 2>&1 && make -j"$JOBS" > make.log 2>&1 ) || {
	echo "compat-check: building HEAD failed; see $out/head-build" >&2
	exit 2
}
libs=$(sed -n 's/^LIBS[[:space:]]*=//p' "$out/head-build/Makefile")
git show "$base:$consumer" > "$out/consumer.c"
# shellcheck disable=SC2086  # $libs is a word list
$CC -std=c11 -D_GNU_SOURCE -g -I"$out/base-prefix/include" \
    -c -o "$out/consumer.o" "$out/consumer.c" &&
    $CC -o "$out/consumer-static" "$out/consumer.o" \
        "$out/head-build/libxtc.a" -pthread -lm $libs || {
	echo "compat-check: FAIL: $base's consumer does not link against HEAD's libxtc.a" >&2
	exit 1
}
rc=0
"$out/consumer-static" > "$out/static.log" 2>&1 || rc=$?
tail -3 "$out/static.log"
if [ "$rc" -ne 0 ]; then
	echo "compat-check: FAIL: $base consumer against HEAD libxtc.a exited $rc (log $out/static.log)"
	exit 1
fi
if ls "$out"/head-build/libxtc.so.* >/dev/null 2>&1; then
	# shellcheck disable=SC2086
	$CC -o "$out/consumer-shared" "$out/consumer.o" -L"$out/head-build" \
	    -lxtc -pthread -lm $libs &&
	    LD_LIBRARY_PATH="$out/head-build" "$out/consumer-shared" \
	        > "$out/shared.log" 2>&1 || {
		tail -3 "$out/shared.log"
		echo "compat-check: FAIL: $base consumer against HEAD libxtc.so (log $out/shared.log)"
		exit 1
	}
	tail -1 "$out/shared.log"
fi
echo "compat-check: OK: $base consumer runs against HEAD's library"
