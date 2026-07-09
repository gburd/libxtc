/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_str.c
 *	Bounded string copy/cat with OpenBSD strlcpy/strlcat semantics.
 *	strncpy(3) silently omits the NUL terminator when the source
 *	fills the buffer, and strncat(3)'s count is the number of source
 *	bytes to append rather than the buffer size -- both classic
 *	buffer footguns.  These always NUL-terminate (when dstsize > 0)
 *	and return the total length the copy/cat TRIED to create, so a
 *	return value >= dstsize is the truncation signal.  Pure; no
 *	allocation, no locking.  Adapted from the public-domain OpenBSD
 *	implementations.
 */

#include "xtc_int.h"

#include <string.h>

#include "os_sharp.h"

/*
 * PUBLIC: size_t __os_strlcpy __P((char *, const char *, size_t));
 */
size_t
__os_strlcpy(char *dst, const char *src, size_t dstsize)
{
	const char *osrc = src;
	size_t nleft = dstsize;

	/* Copy as many bytes as will fit. */
	if (nleft != 0) {
		while (--nleft != 0) {
			if ((*dst++ = *src++) == '\0')
				break;
		}
	}

	/* Not enough room in dst: add NUL and traverse the rest of src. */
	if (nleft == 0) {
		if (dstsize != 0)
			*dst = '\0';   /* NUL-terminate dst */
		while (*src++)
			;
	}

	return (size_t)(src - osrc - 1);   /* count does not include NUL */
}

/*
 * PUBLIC: size_t __os_strlcat __P((char *, const char *, size_t));
 */
size_t
__os_strlcat(char *dst, const char *src, size_t dstsize)
{
	const char *odst = dst;
	const char *osrc = src;
	size_t n = dstsize;
	size_t dlen;

	/* Find the end of dst and the remaining room. */
	while (n-- != 0 && *dst != '\0')
		dst++;
	dlen = (size_t)(dst - odst);
	n = dstsize - dlen;

	/* dst has no NUL within dstsize: nothing to append; report the
	 * length it "tried" to create as dstsize + strlen(src). */
	if (n == 0)
		return dlen + strlen(src);

	while (*src != '\0') {
		if (n != 1) {
			*dst++ = *src;
			n--;
		}
		src++;
	}
	*dst = '\0';

	return dlen + (size_t)(src - osrc);   /* count does not include NUL */
}
