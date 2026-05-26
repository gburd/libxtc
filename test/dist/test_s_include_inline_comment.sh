#!/bin/sh
# test/dist/test_s_include_inline_comment.sh — T6
# Verifies that one-line /* PUBLIC: int foo __P((void)); */ comments
# produce a clean prototype (no trailing "*/" leak).

set -eu
: "${XTC_SRC_DIR:?}"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/src/inc" "$tmp/src/zzz" "$tmp/dist"
cp "$XTC_SRC_DIR/dist/s_include"   "$tmp/dist/"
cp "$XTC_SRC_DIR/dist/gen_inc.awk" "$tmp/dist/"
chmod +x "$tmp/dist/s_include"

cat > "$tmp/src/zzz/zzz.c" <<'EOF'
/* PUBLIC: int zzz_one __P((int)); */
int zzz_one(int x) { return x; }

/*
 * PUBLIC: int zzz_two __P((const char *));
 */
int zzz_two(const char *s) { (void)s; return 0; }
EOF

(cd "$tmp" && dist/s_include >/dev/null)

H="$tmp/src/inc/zzz_ext.h"
[ -f "$H" ] || { echo "  [T6] FAIL: zzz_ext.h not produced" >&2; exit 1; }

if grep -E '^[A-Za-z_].*\*/' "$H"; then
	echo "  [T6] FAIL: inline */ leaked into a prototype line" >&2
	cat "$H" >&2
	exit 1
fi

grep -q '^int zzz_one __P((int));$' "$H" || {
	echo "  [T6] FAIL: zzz_one prototype incorrect"; cat "$H" >&2; exit 1; }
grep -q '^int zzz_two __P((const char \*));$' "$H" || {
	echo "  [T6] FAIL: zzz_two prototype incorrect"; cat "$H" >&2; exit 1; }

echo "  [T6] OK: s_include handles both inline and BDB-style PUBLIC: markers"
