#!/bin/sh
# test/m0/test_docs_abi.sh — D3
set -eu
: "${XTC_SRC_DIR:?}"
F="$XTC_SRC_DIR/docs/abi-stability.md"
[ -f "$F" ] || { echo "  [D3] FAIL: $F missing" >&2; exit 1; }
for token in "SemVer" "PATCH" "MINOR" "MAJOR" "Soft-deprecated" "Deprecated" "Default-off" "Removed"; do
	grep -qi "$token" "$F" || {
		echo "  [D3] FAIL: token '$token' missing from abi-stability.md" >&2
		exit 1
	}
done
echo "  [D3] OK: abi-stability.md covers SemVer + 5-stage deprecation"
