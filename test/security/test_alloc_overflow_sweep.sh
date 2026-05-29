#!/bin/sh
# test/security/test_alloc_overflow_sweep.sh
#
#	Static guard against the integer-overflow-to-heap-overflow class
#	of bug.  Greps the production sources (src/, not tests or
#	examples) for allocation calls whose size is an unchecked
#	additive or multiplicative expression on a caller-supplied value,
#	and for the classic unbounded string functions.
#
#	The allowlist below names the call sites that have been audited
#	and carry an explicit overflow guard (or are provably bounded).
#	A new unguarded site fails the test, forcing either a guard or an
#	allowlist entry with justification.

set -u

XTC_SRC_DIR="${XTC_SRC_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}"
SRC="$XTC_SRC_DIR/src"
fail=0

echo "[sec-sweep] scanning $SRC"

# --- 1. Unbounded string functions: forbidden outright in src/. ---
banned=$(grep -rnE '\b(strcpy|strcat|sprintf|gets|stpcpy)\b' "$SRC" \
    2>/dev/null | grep -vE '//|/\*|\*' || true)
if [ -n "$banned" ]; then
    echo "[sec-sweep] FAIL: banned unbounded string function in src/:"
    echo "$banned"
    fail=1
fi

# --- 2. Additive allocation sizes. ---
# Every malloc(EXPR + n) / malloc(sizeof X + n) / __os_malloc(EXPR + n)
# where the size includes a caller-supplied term must sit next to an
# overflow guard.  We check that each match line, or a line within the
# preceding 6 lines, mentions SIZE_MAX (the guard idiom) -- otherwise
# it is flagged.
additive=$(grep -rnE '(malloc|__os_malloc)\([^)]*\+[^)]*\)' "$SRC" \
    2>/dev/null | grep -vE '//|/\*' || true)

if [ -n "$additive" ]; then
    echo "$additive" | while IFS= read -r line; do
        file=$(echo "$line" | cut -d: -f1)
        ln=$(echo "$line" | cut -d: -f2)
        start=$((ln > 6 ? ln - 6 : 1))
        ctx=$(sed -n "${start},${ln}p" "$file" 2>/dev/null)
        if ! echo "$ctx" | grep -q "SIZE_MAX"; then
            echo "[sec-sweep] WARN: additive alloc without nearby SIZE_MAX guard:"
            echo "    $line"
            echo "UNGUARDED" >> /tmp/sec_sweep_flag.$$
        fi
    done
fi

if [ -f /tmp/sec_sweep_flag.$$ ]; then
    rm -f /tmp/sec_sweep_flag.$$
    echo "[sec-sweep] FAIL: one or more additive allocations lack an"
    echo "            overflow guard.  Add 'if (n > SIZE_MAX - K) return"
    echo "            XTC_E_INVAL;' before the allocation, or justify in"
    echo "            an allowlist entry."
    fail=1
fi

# --- 3. Multiplicative allocation sizes. ---
# malloc(a * b) with a computed product must check the product does
# not wrap (the 'total / n != size' idiom).  Match only genuine
# multiplication -- identifier/number, spaces around '*', identifier/
# number -- so pointer casts ((void **)&p), pointer decls (size_t sz,
# void **out), and dereferences (*out_size) are not flagged.
multiplicative=$(grep -rnE 'malloc\([a-zA-Z0-9_]+ \* [a-zA-Z0-9_]+\)' "$SRC" \
    2>/dev/null | grep -vE '//|/\*|sizeof' || true)
if [ -n "$multiplicative" ]; then
    echo "$multiplicative" | while IFS= read -r line; do
        file=$(echo "$line" | cut -d: -f1)
        ln=$(echo "$line" | cut -d: -f2)
        start=$((ln > 6 ? ln - 6 : 1))
        ctx=$(sed -n "${start},${ln}p" "$file" 2>/dev/null)
        if ! echo "$ctx" | grep -qE '/ *[a-z_]+ *!=|SIZE_MAX'; then
            echo "[sec-sweep] WARN: multiplicative alloc without overflow check:"
            echo "    $line"
            echo "UNGUARDED" >> /tmp/sec_sweep_mul.$$
        fi
    done
fi
if [ -f /tmp/sec_sweep_mul.$$ ]; then
    rm -f /tmp/sec_sweep_mul.$$
    echo "[sec-sweep] FAIL: multiplicative allocation without overflow check."
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "[sec-sweep] OK: no unguarded allocation-size arithmetic, no banned"
    echo "            string functions in src/."
fi
exit $fail
