#!/bin/sh
# test/m0/test_man_signatures.sh -- D6
# Every function man page must mention every parameter name from xtc.h
# and document the return contract.
set -eu
: "${XTC_SRC_DIR:?}"

H="$XTC_SRC_DIR/src/inc/xtc.h"
MANDIR="$XTC_SRC_DIR/man/man3"

# Build (function, params) tuples by parsing single-line declarations.
# Multi-line decls would need a real parser; for M0 every public
# function fits on one logical line.
awk '
	/^[A-Za-z_].*xtc_[A-Za-z0-9_]+\(/ {
		# isolate the parenthesized argument list
		s = $0
		sub(/.*\(/, "", s)
		sub(/\).*/, "", s)
		# function name
		match($0, /xtc_[A-Za-z0-9_]+/)
		name = substr($0, RSTART, RLENGTH)
		# normalize commas
		gsub(/,/, " ", s)
		# print one line: function arg arg ...
		printf "%s", name
		n = split(s, a, /[ \t]+/)
		for (i = 1; i <= n; i++) {
			t = a[i]
			# strip trailing punctuation, asterisks, etc.
			gsub(/[*&;]/, "", t)
			# parameter names are the last identifier in each
			# comma-separated declaration.  We approximate by
			# emitting every alphabetic token; the man page
			# is required to mention at least one per arg.
			if (t ~ /^[A-Za-z_][A-Za-z_0-9]*$/ && t != "void" \
			    && t != "int" && t != "char" && t != "const" \
			    && t != "struct" && t != "long" && t != "short" \
			    && t != "unsigned" && t != "signed" \
			    && t != "size_t" && t != "double" && t != "float") {
				printf " %s", t
			}
		}
		printf "\n"
	}
' "$H" | while read -r fn rest; do
	page="$MANDIR/$fn.3"
	if [ ! -f "$page" ]; then
		continue   # coverage test catches this
	fi
	if ! grep -qiE "(RETURN VALUE|RETURNS)" "$page"; then
		echo "  [D6] FAIL: $fn.3 has no RETURN VALUE section" >&2
		exit 1
	fi
	for tok in $rest; do
		if ! grep -q "$tok" "$page"; then
			echo "  [D6] FAIL: $fn.3 does not mention parameter '$tok'" >&2
			exit 1
		fi
	done
done
echo "  [D6] OK: every man page mentions its parameters and return contract"
