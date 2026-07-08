#!/bin/sh
# test/m0/test_docs_arch.sh -- D2
set -eu
: "${XTC_SRC_DIR:?}"
F="$XTC_SRC_DIR/docs/ARCHITECTURE.md"
if [ ! -f "$F" ]; then
	echo "  [D2] FAIL: $F missing" >&2; exit 1
fi
# ARCHITECTURE.md must document the five implemented layers, L0..L4.
# (L5 pg/ was a design-only adapter and is intentionally not shipped
# documentation; the earlier PLAN.md cross-reference was removed when
# the design memos moved to .agent/.)
for layer in "L0" "L1" "L2" "L3" "L4"; do
	grep -q "$layer" "$F" || { echo "  [D2] FAIL: layer $layer missing from ARCHITECTURE.md" >&2; exit 1; }
done
echo "  [D2] OK: ARCHITECTURE.md lists layers L0-L4"
