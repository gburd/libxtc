#!/bin/sh
# test/dist/test_s_include_idempotent.sh — T3
set -eu
: "${XTC_SRC_DIR:?}"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/src/inc" "$tmp/src/bar" "$tmp/dist"
cp "$XTC_SRC_DIR/dist/s_include"   "$tmp/dist/"
cp "$XTC_SRC_DIR/dist/gen_inc.awk" "$tmp/dist/"
chmod +x "$tmp/dist/s_include"

cat > "$tmp/src/bar/bar.c" <<'EOF'
/* PUBLIC: int bar_one __P((void)); */
int bar_one(void) { return 0; }
EOF

cd "$tmp"
dist/s_include >/dev/null
H="src/inc/bar_ext.h"
sum1=$(sha256sum "$H" | awk '{print $1}')
dist/s_include >/dev/null
sum2=$(sha256sum "$H" | awk '{print $1}')

if [ "$sum1" != "$sum2" ]; then
	echo "  [T3] FAIL: s_include is not idempotent" >&2
	echo "  first:  $sum1" >&2
	echo "  second: $sum2" >&2
	exit 1
fi
echo "  [T3] OK: s_include is idempotent"
