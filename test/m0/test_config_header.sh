#!/bin/sh
# test/m0/test_config_header.sh
# Verifies M0_CLAIMS.md [B7]: xtc_config.h defines the four version macros.

set -eu

: "${XTC_BUILD_DIR:?XTC_BUILD_DIR must be set}"
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
echo "  [B7] OK: xtc_config.h defines all four version macros"
