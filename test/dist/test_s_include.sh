#!/bin/sh
# test/dist/test_s_include.sh -- T2
# Run s_include against a fixture and diff the output against expected.

set -eu
: "${XTC_SRC_DIR:?}"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Build a tiny fake repo with one PUBLIC: marker.
mkdir -p "$tmp/src/inc" "$tmp/src/foo" "$tmp/dist"
cp "$XTC_SRC_DIR/dist/s_include"     "$tmp/dist/"
cp "$XTC_SRC_DIR/dist/gen_inc.awk"   "$tmp/dist/"
cp "$XTC_SRC_DIR/dist/s_perm"        "$tmp/dist/"
chmod +x "$tmp/dist"/s_*

cat > "$tmp/src/foo/foo_one.c" <<'EOF'
/* PUBLIC: int foo_one __P((int *)); */
int foo_one(int *p) { return p ? *p : 0; }
EOF

cat > "$tmp/src/foo/foo_two.c" <<'EOF'
/* PUBLIC: int foo_two __P((const char *, int *)); */
int foo_two(const char *s, int *o) { (void)s; *o = 1; return 0; }
EOF

(cd "$tmp" && dist/s_include >/dev/null)

H="$tmp/src/inc/foo_ext.h"
[ -f "$H" ] || { echo "  [T2] FAIL: foo_ext.h not produced" >&2; exit 1; }
grep -q "int foo_one __P((int \*));" "$H" || {
	echo "  [T2] FAIL: foo_one prototype missing"; cat "$H"; exit 1; }
grep -q "int foo_two __P((const char \*, int \*));" "$H" || {
	echo "  [T2] FAIL: foo_two prototype missing"; cat "$H"; exit 1; }
grep -q "DO NOT EDIT" "$H" || {
	echo "  [T2] FAIL: header missing the warning banner"; exit 1; }

echo "  [T2] OK: s_include produces a correct prototype header"
