/*-
 * Copyright (c) 2026, The XTC Project
 *
 * Use of this source code is governed by the ISC License.
 *
 * src/xtc_version.c
 *	Implementation of the M0-public version API.
 *	The version is set at configure time; see dist/configure.ac and
 *	the substitution into xtc_config.h.
 */

#include <stddef.h>
#include <stdio.h>

#include "xtc.h"

/*
 * The three integer components are the ABI level (dist/version.in)
 * and the string is the release name (dist/release_name.sh: the ABI
 * level, or a CalVer date for a development release).  The macros
 * come from xtc_config.h, which configure or meson generates.
 *
 * PUBLIC: const char *xtc_version_string __P((void));
 */
const char *
xtc_version_string(void)
{
	return XTC_VERSION_STRING;
}

/*
 * PUBLIC: int xtc_version_components __P((int *, int *, int *));
 */
int
xtc_version_components(int *major, int *minor, int *patch)
{
	if (major == NULL || minor == NULL || patch == NULL)
		return XTC_E_INVAL;
	*major = XTC_VERSION_MAJOR;
	*minor = XTC_VERSION_MINOR;
	*patch = XTC_VERSION_PATCH;
	return XTC_OK;
}
