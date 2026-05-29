#!/bin/sh
# test/dist/test_amalgamation.sh
#
#	Generate the single-file amalgamation (xtc.c + xtc.h), compile
#	it, link a tiny program against it, and verify the embedded
#	version identity.  Also checks the debug #line remapping:
#	with -DDEBUG -DXTC_RELATIVE_LOC=PFX a diagnostic must point at
#	PFX/src/... rather than xtc.c.

set -eu

XTC_SRC_DIR="${XTC_SRC_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}"

if ! command -v python3 >/dev/null 2>&1; then
	echo "  [amalgamation] SKIP: python3 not on PATH"
	exit 0
fi

CC="${CC:-cc}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

python3 "$XTC_SRC_DIR/dist/mkamalgamation.py" \
	--root "$XTC_SRC_DIR" --out "$work" >/dev/null

test -s "$work/xtc.c" || { echo "  [amalgamation] FAIL: no xtc.c"; exit 1; }
test -s "$work/xtc.h" || { echo "  [amalgamation] FAIL: no xtc.h"; exit 1; }

# --- compile the amalgamation (default substrate = ucontext) ---
if ! $CC -std=c11 -D_GNU_SOURCE -c "$work/xtc.c" -o "$work/xtc.o" \
	2> "$work/cc.err"; then
	echo "  [amalgamation] FAIL: xtc.c did not compile"
	head -20 "$work/cc.err" >&2
	exit 1
fi

# --- link a demo that uses a real symbol + the version macros ---
cat > "$work/demo.c" <<'EOF'
#include "xtc.h"
#include <string.h>
#include <stdlib.h>
int main(void) {
	if (strcmp(xtc_version_string(), XTC_VERSION_STRING) != 0) return 2;
	if (!XTC_AMALGAMATION) return 3;
	/* XTC_VERSION_COMMIT_SHORT must be a (possibly empty) string. */
	(void)XTC_VERSION_COMMIT_SHORT;
	(void)XTC_VERSION_COMMIT_LONG;
	(void)XTC_VERSION_TAG;
	return 0;
}
EOF

LIBS="-pthread"
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists liburing 2>/dev/null; then
	LIBS="$LIBS $(pkg-config --libs liburing)"
fi
# OpenSSL only needed if the amalgamation pulled a TLS backend; the
# amalgamation has no xtc_config.h so TLS is off -- but link defensively.
if pkg-config --exists libssl 2>/dev/null; then
	LIBS="$LIBS $(pkg-config --libs libssl libcrypto 2>/dev/null)"
fi

if ! $CC -std=c11 -D_GNU_SOURCE -I"$work" "$work/demo.c" "$work/xtc.o" \
	-o "$work/demo" $LIBS 2> "$work/link.err"; then
	echo "  [amalgamation] FAIL: demo did not link"
	head -20 "$work/link.err" >&2
	exit 1
fi

if ! "$work/demo"; then
	echo "  [amalgamation] FAIL: demo returned nonzero (version mismatch)"
	exit 1
fi

# --- #line remapping: a diagnostic under the debug macros must name
#     the remapped prefix, not xtc.c. ---
remap_ok=skip
if $CC -std=c11 -D_GNU_SOURCE -Wall -DDEBUG \
	-DXTC_RELATIVE_LOC=__amalgtest__ -c "$work/xtc.c" \
	-o /dev/null 2> "$work/warn.err"; then
	:
fi
if grep -q "__amalgtest__/src/" "$work/warn.err" 2>/dev/null; then
	remap_ok=yes
fi

echo "  [amalgamation] OK: xtc.c+xtc.h compile, demo links and runs;" \
     "version $(grep -m1 XTC_VERSION_STRING "$work/xtc.h" | sed 's/.*\"\(.*\)\".*/\1/');" \
     "line-remap=$remap_ok"
exit 0
