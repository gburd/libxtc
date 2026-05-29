#!/usr/bin/env bash
#-
# dist/santorini-matrix.sh
#
#	Build and test libxtc on the santorini Windows 11 host across
#	every available toolchain.  Intended to run inside the MSYS2
#	bash shell.  Reports a per-toolchain summary.
#
#	The source tree is expected at /c/scratch/xtc (see
#	docs/M_WINDOWS_MATRIX.md for how to land it there).
#
#	Usage, from a workstation:
#	  scp dist/santorini-matrix.sh santorini:matrix.sh
#	  ssh santorini 'cmd /c "C:\msys64\usr\bin\bash.exe -l ~/matrix.sh"'

set -u

SRC=/c/scratch/xtc
if [ ! -d "$SRC" ]; then
	echo "source tree not found at $SRC" >&2
	exit 1
fi

# Toolchain table: name, PATH prefix, CC.
run_toolchain() {
	local name=$1 path_prefix=$2 cc=$3 build_dir=$4
	echo "================================================================"
	echo "Toolchain: $name"
	echo "================================================================"

	if [ ! -x "$cc" ] && ! PATH="$path_prefix:$PATH" command -v "$cc" >/dev/null 2>&1; then
		echo "  SKIP: compiler not found ($cc)"
		return
	fi

	(
		export PATH="$path_prefix:/usr/bin:$PATH"
		cd "$SRC" || exit 1
		[ -d "$build_dir" ] || mkdir "$build_dir"
		cd "$build_dir" || exit 1

		"$cc" --version 2>/dev/null | head -1
		"$cc" -dumpmachine 2>/dev/null

		[ -f Makefile ] || CC="$cc" ../dist/configure --with-tls=none \
			> configure.log 2>&1

		if ! make libxtc.a > build.log 2>&1; then
			echo "  FAIL: libxtc.a did not build; see $build_dir/build.log"
			grep -E "error:" build.log | head -5
			exit 1
		fi
		echo "  libxtc.a: OK"

		# Build every C test target; -k keeps going past the few
		# POSIX-only ones that don't compile on Windows.
		make -k tests-c > tests-build.log 2>&1
		# shellcheck disable=SC2012  # test names are fixed and simple
		built=$(ls test_*.exe 2>/dev/null | wc -l)
		echo "  test binaries built: $built"

		local pass=0 skip=0 fail=0
		for t in test_*.exe; do
			[ -x "$t" ] || continue
			out=$(./"$t" 2>&1 | tail -1)
			case "$out" in
				*100%*)   pass=$((pass+1)) ;;
			esac
			case "$out" in
				*SKIP*|*skipped*) skip=$((skip+1)) ;;
			esac
			case "$out" in
				*[Ff]ail*|*[Ee]rror*) fail=$((fail+1));
				    echo "    FAIL $t : $out" ;;
			esac
		done
		echo "  results: pass=$pass skip=$skip fail=$fail"
	)
}

run_toolchain "MinGW64 gcc" "/mingw64/bin"        gcc   build_mingw
run_toolchain "Clang64"     "/c/msys64/clang64/bin" clang build_clang64

# MSVC requires the asm port + meson expansion (see
# docs/M_WINDOWS_MATRIX.md); not driven by this script yet.
echo "================================================================"
echo "MSVC: not driven by this script (asm port pending)."
echo "================================================================"
