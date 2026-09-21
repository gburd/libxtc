#!/bin/sh
# test/m0/test_docs_abi.sh -- D3: docs/abi-stability.md must describe
# machinery that EXISTS.
#
# History: this gate used to grep the page for the words "SemVer",
# "PATCH", "MINOR", ... and pass.  It therefore passed while the page
# described five files that were not in the tree (dist/s_abi,
# dist/pubdef.in, dist/capabilities.in, test/compat/, test/trace_compat/)
# and an API that was in no header (xtc_have_capability).  A gate that
# cannot fail is not a gate.
#
# What it checks now:
#   [D3a] every repo-relative path the page cites in backticks exists.
#   [D3b] every xtc_*() / __os_*() function the page names in backticks
#         is declared in an installed public header (or is explicitly
#         disclaimed on the same line -- see NEGATED below).
#   [D3c] the page does not resurrect any of the specific false claims
#         that were removed, unless the thing it names now exists.
#   [D3d] the policy vocabulary is still present (the old check, kept:
#         it is cheap and catches a page gutted by accident).
set -eu
: "${XTC_SRC_DIR:?}"
SRC="$XTC_SRC_DIR"
F="$SRC/docs/abi-stability.md"
[ -f "$F" ] || { echo "  [D3] FAIL: $F missing" >&2; exit 1; }

rc=0

# ---------------------------------------------------------------- D3a
# Paths the page cites, in backticks, that look repo-relative: a
# known top-level directory followed by a path.  Trailing punctuation
# and a trailing / are stripped.  A path containing a shell/glob
# metacharacter or an ALL-CAPS placeholder is skipped (it is a pattern,
# e.g. `xtc_*` or `libxtc.so.MAJOR`, not a file).
# shellcheck disable=SC2016  # the backtick is markdown, not a subshell
paths=$(grep -oE '`(dist|src|test|man|docs|tools|scripts|examples)/[A-Za-z0-9_./*-]+`' "$F" |
	tr -d '`' | sed 's,/*$,,' | sort -u || true)
for p in $paths; do
	case "$p" in
	*'*'*|*'?'*|*MAJOR*|*FULL*) continue ;;
	esac
	if [ ! -e "$SRC/$p" ]; then
		echo "  [D3a] FAIL: abi-stability.md cites '$p', which is not in the tree" >&2
		rc=1
	fi
done
[ "$rc" -eq 0 ] && echo "  [D3a] OK: all $(echo "$paths" | wc -l | tr -d ' ') cited paths exist"

# ---------------------------------------------------------------- D3b
# Functions the page names.  A line that DISCLAIMS a function (says it
# does not exist / is absent / never compiled) is allowed to name it --
# that is the honest way to retract a former claim.
NEGATED='no such|does not exist|has no|never compiled|is absent|not present|no capability|removed|there is no'
# shellcheck disable=SC2016  # the backtick is markdown, not a subshell
fns=$(grep -oE '`(xtc|__os)_[A-Za-z0-9_]+\(\)?`' "$F" | tr -d '`()' | sort -u || true)
for fn in $fns; do
	if grep -qE "\b$fn\b" "$SRC"/src/inc/*.h 2>/dev/null; then
		continue
	fi
	# Not declared anywhere.  Every line mentioning it must disclaim it.
	bad=$(grep -n "$fn" "$F" | grep -viE "$NEGATED" || true)
	if [ -n "$bad" ]; then
		echo "  [D3b] FAIL: abi-stability.md names '$fn', which is declared in no public header:" >&2
		echo "$bad" | sed 's/^/        /' >&2
		rc=1
	fi
done
[ "$rc" -eq 0 ] && echo "  [D3b] OK: every named API exists or is explicitly disclaimed"

# ---------------------------------------------------------------- D3c
# The specific machinery that was once claimed and does not exist.  If
# someone BUILDS one of these, the check passes automatically (the path
# exists); until then the page must not claim it.
# $thing is an EXTENDED REGEX; anchor it with \b where a bare substring
# would false-positive (e.g. 's_abi' inside this script's own name, which
# the page legitimately cites).
check_absent_claim() {
	thing=$1 kind=$2 target=$3
	case "$kind" in
	path) [ -e "$SRC/$target" ] && return 0 ;;
	sym)  grep -qE "\b$target\b" "$SRC"/src/inc/*.h 2>/dev/null && return 0 ;;
	esac
	hits=$(grep -nE "$thing" "$F" | grep -viE "$NEGATED" || true)
	if [ -n "$hits" ]; then
		echo "  [D3c] FAIL: '$thing' does not exist ($kind $target absent), but is claimed:" >&2
		echo "$hits" | sed 's/^/        /' >&2
		rc=1
	fi
}
check_absent_claim '(^|[^_a-z])s_abi\b'   path 'dist/s_abi'
check_absent_claim 'pubdef\.in'           path 'dist/pubdef.in'
check_absent_claim 'capabilities\.in'     path 'dist/capabilities.in'
check_absent_claim 'test/compat'          path 'test/compat'
check_absent_claim 'trace_compat'         path 'test/trace_compat'
check_absent_claim 'xtc_have_capability'  sym  'xtc_have_capability'
check_absent_claim 'xtc-migrate'          path 'tools/xtc-migrate-1to2'
check_absent_claim 'xtcadmin'             path 'tools/xtcadmin'
check_absent_claim 'xtcdump'              path 'tools/xtcdump'
# .symver: claiming per-symbol versioning while libxtc.map is a single
# unnamed node (its own comment says the symbols stay UNVERSIONED).
if ! grep -qE '^[[:space:]]*XTC_[0-9]' "$SRC/dist/libxtc.map" 2>/dev/null; then
	hits=$(grep -n 'symver' "$F" | grep -viE "$NEGATED" || true)
	if [ -n "$hits" ]; then
		echo "  [D3c] FAIL: dist/libxtc.map has no named version node, so .symver" >&2
		echo "             per-symbol versioning must not be claimed:" >&2
		echo "$hits" | sed 's/^/        /' >&2
		rc=1
	fi
fi
[ "$rc" -eq 0 ] && echo "  [D3c] OK: no absent machinery claimed"

# ---------------------------------------------------------------- D3d
for token in "SemVer\|semver\|MAJOR.MINOR.PATCH" "PATCH" "MINOR" "MAJOR" \
             "Soft-deprecated" "Deprecated" "Default-off" "Removed" \
             "ENFORCED" "POLICY"; do
	grep -qi "$token" "$F" || {
		echo "  [D3d] FAIL: token '$token' missing from abi-stability.md" >&2
		rc=1
	}
done
[ "$rc" -eq 0 ] && echo "  [D3d] OK: version policy + deprecation stages + enforced/policy split all documented"

[ "$rc" -eq 0 ] || { echo "  [D3] FAIL: abi-stability.md claims things the tree does not have" >&2; exit 1; }
echo "  [D3] OK: abi-stability.md describes only machinery that exists"
