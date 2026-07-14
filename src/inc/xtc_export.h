/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/xtc_export.h
 *	Cross-platform shared-library symbol export/import macro.
 *
 *	MSVC does not export ANY symbol from a DLL by default (unlike
 *	gcc/clang, which export everything from a .so unless told
 *	otherwise); every function meant to be callable from outside
 *	the DLL needs an explicit __declspec(dllexport) on the
 *	declaration seen while COMPILING the library, and
 *	__declspec(dllimport) on the declaration seen while COMPILING
 *	a CONSUMER that links against the DLL.  XTC_API expands to the
 *	right one of those, or to nothing at all everywhere else:
 *
 *	  - XTC_BUILDING_DLL defined (set by the library's own build
 *	    when compiling a Windows shared/DLL target): dllexport.
 *	  - XTC_DLL defined, XTC_BUILDING_DLL NOT defined (a consumer
 *	    that has opted into "I am linking against xtc as a DLL"):
 *	    dllimport.
 *	  - Anywhere else -- non-Windows (gcc/clang .so, static libs on
 *	    every platform, or a Windows consumer of the static
 *	    xtc.lib that never defines XTC_DLL): nothing.  This is the
 *	    common case and MUST be a complete no-op there.
 *
 *	A consumer building against the shared xtc.dll on Windows adds
 *	-DXTC_DLL to its own compile flags; everyone else needs no
 *	flag at all.
 */

#ifndef XTC_EXPORT_H
#define XTC_EXPORT_H

#if defined(_WIN32) && defined(XTC_BUILDING_DLL)
#  define XTC_API __declspec(dllexport)
#elif defined(_WIN32) && defined(XTC_DLL)
#  define XTC_API __declspec(dllimport)
#else
#  define XTC_API
#endif

#endif /* XTC_EXPORT_H */
