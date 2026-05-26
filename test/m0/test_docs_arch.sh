#!/bin/sh
# test/m0/test_docs_arch.sh — D2
set -eu
: "${XTC_SRC_DIR:?}"
F="$XTC_SRC_DIR/docs/ARCHITECTURE.md"
if [ ! -f "$F" ]; then
	echo "  [D2] FAIL: $F missing" >&2; exit 1
fi
grep -q "PLAN.md" "$F" || { echo "  [D2] FAIL: ARCHITECTURE does not reference PLAN.md" >&2; exit 1; }
for layer in "L0" "L1" "L2" "L3" "L4" "L5"; do
	grep -q "$layer" "$F" || { echo "  [D2] FAIL: layer $layer missing" >&2; exit 1; }
done
echo "  [D2] OK: ARCHITECTURE.md references PLAN.md and lists all six layers"
