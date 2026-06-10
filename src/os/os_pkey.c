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
#include <cpuid.h>

/*
 * True only when the OS has actually enabled protection keys in CR4
 * (CPUID.(EAX=7,ECX=0).ECX bit 4, OSPKE).  pkey_alloc() succeeding is
 * NOT sufficient: the kernel can support the syscall while CR4.PKE is
 * clear (notably under hypervisors that expose the PKU CPUID bit but
 * not OSPKE), and then the WRPKRU instruction that glibc's pkey_set()
 * executes in userspace faults with SIGILL.  Gate on OSPKE so we never
 * issue WRPKRU on a CPU/OS that has not enabled it.
 */
static int
x86_ospke(void)
{
	unsigned eax, ebx, ecx, edx;

	if (__get_cpuid_max(0, NULL) < 7)
		return 0;
	__cpuid_count(7, 0, eax, ebx, ecx, edx);
	return (ecx & (1u << 4)) != 0;   /* OSPKE: CR4.PKE is set */
}

int
xtc_pkey_supported(void)
{
	int k;

	if (!x86_ospke())
		return 0;             /* PKU not enabled by the OS -- WRPKRU would trap */
	k = pkey_alloc(0, 0);
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
	if (!x86_ospke())
		return XTC_E_NOSYS;   /* keys unusable without OSPKE (WRPKRU traps) */
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
