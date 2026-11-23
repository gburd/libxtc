#!/bin/sh
# test/m0/test_config_header.sh
# Verifies that xtc_config.h defines the four version macros, that
# MAJOR/MINOR/PATCH are the ABI level in dist/version.in, and that
# XTC_VERSION_STRING is the release name dist/release_name.sh gives.

set -eu

: "${XTC_BUILD_DIR:?XTC_BUILD_DIR must be set}"
: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set}"
H="$XTC_BUILD_DIR/xtc_config.h"

if [ ! -f "$H" ]; then
	echo "  [B7] FAIL: $H missing" >&2
	exit 1
fi

for sym in XTC_VERSION_STRING XTC_VERSION_MAJOR XTC_VERSION_MINOR XTC_VERSION_PATCH; do
	if ! grep -q "^#define $sym" "$H"; then
		echo "  [B7] FAIL: $sym not defined in xtc_config.h" >&2
		grep "$sym" "$H" >&2 || true
		exit 1
	fi
done

macro() {
	sed -n "s/^#define $1 //p" "$H"
}
abi=$(tr -d '[:space:]' < "$XTC_SRC_DIR/dist/version.in")
name=$(sh "$XTC_SRC_DIR/dist/release_name.sh" "$XTC_SRC_DIR/dist")
got_abi="$(macro XTC_VERSION_MAJOR).$(macro XTC_VERSION_MINOR).$(macro XTC_VERSION_PATCH)"
if [ "$got_abi" != "$abi" ]; then
	echo "  [B7] FAIL: MAJOR.MINOR.PATCH is $got_abi, dist/version.in says $abi" >&2
	exit 1
fi
if [ "$(macro XTC_VERSION_STRING)" != "\"$name\"" ]; then
	echo "  [B7] FAIL: XTC_VERSION_STRING is $(macro XTC_VERSION_STRING)," \
	    "the release name is \"$name\"" >&2
	exit 1
fi

# The MSVC build does not run configure: its hand-kept header must say
# the same, or a Windows build of a release reports another version.
C="$XTC_SRC_DIR/src/inc/compat/xtc_config.h"
cmacro() {
	sed -n "s/^#define $1 *//p" "$C"
}
got_abi="$(cmacro XTC_VERSION_MAJOR).$(cmacro XTC_VERSION_MINOR).$(cmacro XTC_VERSION_PATCH)"
if [ "$got_abi" != "$abi" ] || [ "$(cmacro XTC_VERSION_STRING)" != "\"$name\"" ]; then
	echo "  [B7] FAIL: src/inc/compat/xtc_config.h says $got_abi" \
	    "$(cmacro XTC_VERSION_STRING); want ABI level $abi and \"$name\"" >&2
	exit 1
fi
echo "  [B7] OK: xtc_config.h defines all four version macros" \
    "(ABI level $abi, release name $name), and the MSVC header agrees"
