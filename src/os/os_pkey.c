/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_pkey.c
 *	Memory-protection-key tier.  See xtc_pkey.h.  Real on Linux/x86
 *	(glibc pkey_* wrappers + PKRU); XTC_E_NOSYS everywhere else.
 */

#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#  endif
#endif

#include "xtc_int.h"
#include "xtc_pkey.h"

#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))

#include <sys/mman.h>

int
xtc_pkey_supported(void)
{
	int k = pkey_alloc(0, 0);
	if (k < 0)
		return 0;             /* CPU/kernel without PKU */
	(void)pkey_free(k);
	return 1;
}

int
xtc_pkey_alloc(int *out_key)
{
	int k;
	if (out_key == NULL)
		return XTC_E_INVAL;
	k = pkey_alloc(0, 0);
	if (k < 0)
		return XTC_E_NOSYS;
	*out_key = k;
	return XTC_OK;
}

int
xtc_pkey_protect(void *addr, size_t len, int key)
{
	if (addr == NULL)
		return XTC_E_INVAL;
	if (pkey_mprotect(addr, len, PROT_READ | PROT_WRITE, key) != 0)
		return XTC_E_INTERNAL;
	return XTC_OK;
}

int
xtc_pkey_set_access(int key, int allow_read, int allow_write)
{
	unsigned rights = 0;
	if (!allow_read)
		rights |= PKEY_DISABLE_ACCESS;
	else if (!allow_write)
		rights |= PKEY_DISABLE_WRITE;
	if (pkey_set(key, rights) != 0)
		return XTC_E_INTERNAL;
	return XTC_OK;
}

int
xtc_pkey_free(int key)
{
	return pkey_free(key) == 0 ? XTC_OK : XTC_E_INTERNAL;
}

#else  /* no PKU: portable no-op stubs */

int xtc_pkey_supported(void) { return 0; }
int xtc_pkey_alloc(int *out_key) { (void)out_key; return XTC_E_NOSYS; }
int xtc_pkey_protect(void *a, size_t l, int k)
{ (void)a; (void)l; (void)k; return XTC_E_NOSYS; }
int xtc_pkey_set_access(int k, int ar, int aw)
{ (void)k; (void)ar; (void)aw; return XTC_E_NOSYS; }
int xtc_pkey_free(int k) { (void)k; return XTC_E_NOSYS; }

#endif
