#!/bin/sh
# scripts/ci-check-part.sh BUILD_DIR PART
#
# Run one slice of `make check` in a configured build directory, for a CI
# runner that cannot fit the whole of it in one job (codeberg-small
# cancels a job at 5:00).  The slices come from the Makefile's own lists,
# split by position, so a test added to TESTS_C or TESTS_SH lands in a
# slice without editing the workflow:
#
#   c1 c2 c3 the three thirds of TESTS_C, without the three below
#   exec     test_exec    (alone: its Blk2 stress is ~45 s on 2 CPUs)
#   xproc    test_xproc   (alone: entry_mt_parent is ~36 s on 2 CPUs)
#   reclaim  test_stack_reclaim (alone: 5 s, but 38 s under ASan)
#   sh1      the first half of TESTS_SH
#   sh2      the second half of TESTS_SH, then tests-pbt, check-examples
#   pbt      tests-pbt only (for a build that does not run the shell tier)
#
# c1 c2 c3 exec xproc reclaim sh1 sh2 together run everything `make
# check` runs.
# Each slice builds only what it runs, in parallel, then runs it.
# Env: MAKE (make).
set -eu
B=${1:?usage: ci-check-part.sh BUILD_DIR PART}
PART=${2:?usage: ci-check-part.sh BUILD_DIR PART}
MAKE=${MAKE:-make}
J=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)

# The words of make variable $1 in $B's Makefile, one per line.
lst() {
	# shellcheck disable=SC2016  # $(...) is make syntax, for make
	printf 'ci-print-var:\n\t@echo $(%s)\n' "$1" |
	    $MAKE -s -C "$B" -f Makefile -f - ci-print-var | tr ' ' '\n' |
	    grep .
}
# Part $1 of $2 equal parts of the lines on stdin.
part() {
	_w=$(cat)
	_n=$(printf '%s\n' "$_w" | grep -c .)
	_k=$(((_n + $2 - 1) / $2))
	printf '%s\n' "$_w" | tail -n +"$(((($1 - 1) * _k) + 1))" | head -n "$_k"
}
# Build the words in $1 (a list) in parallel.
build() {
	# shellcheck disable=SC2086  # $1 is a word list
	$MAKE -C "$B" -j"$J" $1
}

case $PART in
c1|c2|c3|exec|xproc|reclaim)
	case $PART in
	exec|xproc) t=test_$PART ;;
	reclaim) t=test_stack_reclaim ;;
	*) t=$(lst TESTS_C |
	       grep -vxE 'test_exec|test_xproc|test_stack_reclaim' |
	       part "${PART#c}" 3 | tr '\n' ' ') ;;
	esac
	echo "== $PART: $(echo "$t" | wc -w) C test(s)"
	build "$t"
	$MAKE -C "$B" tests-c TESTS_C="$t"
	;;
sh1|sh2)
	t=$(lst TESTS_SH | part "${PART#sh}" 2 | tr '\n' ' ')
	echo "== $PART: $(echo "$t" | wc -w) shell test(s)"
	build "libxtc.a tnt_echo"
	$MAKE -C "$B" tests-sh TESTS_SH="$t"
	if [ "$PART" = sh2 ]; then
		build "$(lst TESTS_PBT | tr '\n' ' ')"
		$MAKE -C "$B" tests-pbt
		$MAKE -C "$B" check-examples
	fi
	;;
pbt)
	build "$(lst TESTS_PBT | tr '\n' ' ')"
	$MAKE -C "$B" tests-pbt
	;;
*)
	echo "ci-check-part.sh: unknown part '$PART'" >&2
	exit 2
	;;
esac
