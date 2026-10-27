#!/bin/sh
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# scripts/style-check.sh [BASE] -- clang-format the lines a change touches.
#
# Runs `git clang-format --diff BASE` over the C sources: only the lines
# changed since BASE (default: the merge base with origin/main, else
# HEAD~1) are formatted against .clang-format, and the would-be diff is
# printed.  The tree is NOT formatted to .clang-format and must not be
# reformatted wholesale; this keeps new and edited code in style without
# churning the rest.
#
# Exit 0 when the touched lines are already formatted (or there are no C
# changes), 1 when they are not (the diff is the fix: apply it with
# `git clang-format BASE`), 2 on a setup error.  STYLE_REPORT_ONLY=1
# prints the diff and exits 0 (CI's informational mode).
#
# env: CLANG_FORMAT (the binary; default: clang-format-18, the version CI
#      uses, then -19 .. -21, then
#      plain clang-format), GIT_CLANG_FORMAT (default git-clang-format or a
#      versioned one next to the binary).
set -eu

# shellcheck disable=SC1007
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$src"

pick() {
	for c in "$@"; do
		if command -v "$c" >/dev/null 2>&1; then
			echo "$c"
			return 0
		fi
	done
	return 1
}
CF=${CLANG_FORMAT:-$(pick clang-format-18 clang-format-19 clang-format-20 \
    clang-format-21 clang-format || true)}
v=${CF##*-}
case "$v" in [0-9]*) ;; *) v= ;; esac
GCF=${GIT_CLANG_FORMAT:-$(pick ${v:+git-clang-format-$v} git-clang-format || true)}
if [ -z "$CF" ] || [ -z "$GCF" ]; then
	echo "style-check: SKIP: clang-format / git-clang-format not found" >&2
	exit 2
fi
if ! "$CF" --style=file --dump-config >/dev/null 2>&1; then
	echo "style-check: $CF rejects .clang-format:" >&2
	"$CF" --style=file --dump-config 2>&1 >/dev/null | head -5 >&2
	exit 2
fi

base=${1:-}
if [ -z "$base" ]; then
	base=$(git merge-base HEAD origin/main 2>/dev/null ||
	    git rev-parse HEAD~1 2>/dev/null || true)
fi
if [ -z "$base" ] || ! git rev-parse --verify -q "$base^{commit}" >/dev/null; then
	echo "style-check: no base commit ($base)" >&2
	exit 2
fi

# The vendored Lime submodule and the generated parser are not ours to
# format.
# git-clang-format exits 1 when it has a diff to show and also on its own
# errors, so tell them apart by the output: a real diff starts "diff --git".
err=$(mktemp "${TMPDIR:-/tmp}/style-check.XXXXXX")
trap 'rm -f "$err"' EXIT
out=$("$GCF" --binary "$CF" --style file --extensions c,h --diff \
    "$base" -- src test bench examples tools \
    ':(exclude)examples/06_sqlxtc/lime' \
    ':(exclude)examples/06_sqlxtc/sql_parse_gen.c' \
    ':(exclude)examples/06_sqlxtc/sql_parse_gen.h' 2>"$err") || true
if [ -s "$err" ]; then
	echo "style-check: $GCF failed (pair it with a clang-format of the same version):" >&2
	head -5 "$err" >&2
	exit 2
fi
case "$out" in
'diff --git'*) ;;
*)
	echo "style-check: OK: lines changed since $(git rev-parse --short "$base") are formatted ($("$CF" --version | head -1))"
	exit 0
	;;
esac
printf '%s\n' "$out"
n=$(printf '%s\n' "$out" | grep -c '^+[^+]' || true)
echo "style-check: $n line(s) changed since $(git rev-parse --short "$base") differ from .clang-format; apply with: git clang-format $base"
[ "${STYLE_REPORT_ONLY:-0}" = 1 ] && exit 0
exit 1
