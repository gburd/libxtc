#!/bin/sh
# dist/release_name.sh -- print the release name of this tree.
#
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# Usage: release_name.sh [DIST_DIR]
#
# libxtc carries two versions.  dist/version.in holds the ABI level,
# MAJOR.MINOR.PATCH: it gives XTC_VERSION_MAJOR/_MINOR/_PATCH, the
# SONAME (libxtc.so.MAJOR) and the shared library's real-file name.
# The release name is what a release is called: XTC_VERSION_STRING,
# xtc_version_string(), pkg-config's Version and the release tag
# (v<name>).  An LTS release (lts/2.1) is named by its ABI level, so it
# has no dist/release.in.  A development release from main commits a
# dist/release.in holding its CalVer name, YYYY.MM.DD, or YYYY.MM.DD.N
# for a second release on one day.
#
# Both build systems, the version tests, the release workflow,
# mkamalgamation.py and scripts/compat-check.sh call this script, so the
# rule lives once.  flake.nix cannot run it and reads the two files
# itself, unchecked.
# It prints the name and exits 0, or names the problem and exits 1.
set -eu

dir=${1:-$(dirname -- "$0")}
abi=$(tr -d '[:space:]' < "$dir/version.in")
if [ ! -f "$dir/release.in" ]; then
	echo "$abi"
	exit 0
fi
name=$(tr -d '[:space:]' < "$dir/release.in")
if [ "$name" = "$abi" ]; then
	echo "$name"
	exit 0
fi
# CalVer: a year 2000-2099, a month 01-12, a day that exists in that
# month (February 29 only in a leap year), and an optional .N (N >= 1,
# no leading zero).  The regex checks the shape, awk the day.
if echo "$name" | grep -Eq \
    '^20[0-9]{2}\.(0[1-9]|1[0-2])\.(0[1-9]|[12][0-9]|3[01])(\.[1-9][0-9]*)?$' &&
    echo "$name" | awk -F. '{
	y = $1 + 0; m = $2 + 0; d = $3 + 0
	split("31 28 31 30 31 30 31 31 30 31 30 31", dim, " ")
	if (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))
		dim[2] = 29
	exit !(d <= dim[m])
    }'; then
	echo "$name"
	exit 0
fi
echo "release_name.sh: $dir/release.in holds '$name', which is neither" \
    "the ABI version in version.in ($abi) nor a CalVer name" \
    "YYYY.MM.DD[.N]" >&2
exit 1
