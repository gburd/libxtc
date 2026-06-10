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
#include <sys/wait.h>
#include <cpuid.h>
#include <unistd.h>
#include <pthread.h>

/*
 * Whether protection keys are actually usable is decided ONCE, by a
 * guarded probe.  CPUID OSPKE and a successful pkey_alloc() are both
 * necessary but NOT sufficient: a kernel can support the pkey syscalls
 * (and even report OSPKE) while the WRPKRU instruction that glibc's
 * pkey_set() executes in userspace still faults -- seen on virtualized
 * CI runners.  So a forked child executes a real pkey_set (the WRPKRU
 * path) and _exit(0)s only if it survives; if WRPKRU traps, the child
 * dies on the signal and the parent sees an abnormal status and
 * reports "unsupported".  A child is used rather than a signal handler
 * so a trap can never disturb the caller's process.  Runs at most once.
 */
static int            g_pkey_ok;
static pthread_once_t g_pkey_once = PTHREAD_ONCE_INIT;

static int
x86_ospke(void)
{
	unsigned eax, ebx, ecx, edx;

	if (__get_cpuid_max(0, NULL) < 7)
		return 0;
	__cpuid_count(7, 0, eax, ebx, ecx, edx);
	return (ecx & (1u << 4)) != 0;   /* OSPKE: CR4.PKE is set */
}

static void
pkey_probe(void)
{
	pid_t pid;
	int status, k;

	g_pkey_ok = 0;
	if (!x86_ospke())
		return;                       /* CR4.PKE clear: WRPKRU would trap */
	k = pkey_alloc(0, 0);
	if (k < 0)
		return;                       /* kernel without pkey support */
	(void)pkey_free(k);

	/* OSPKE and the syscall agree; confirm WRPKRU does not trap by
	 * running it in a throwaway child. */
	pid = fork();   /* XTC_BLOCKING_OK: one-shot capability probe, not a hot path */
	if (pid == 0) {
		int ck = pkey_alloc(0, 0);
		if (ck < 0)
			_exit(1);
		(void)pkey_set(ck, 0);        /* WRPKRU; a trap kills only the child */
		_exit(0);                     /* survived: usable */
	} else if (pid > 0) {
		if (waitpid(pid, &status, 0) == pid &&
		    WIFEXITED(status) && WEXITSTATUS(status) == 0)
			g_pkey_ok = 1;
	}
	/* fork failure or any abnormal child exit -> not usable. */
}

int
xtc_pkey_supported(void)
{
	(void)pthread_once(&g_pkey_once, pkey_probe);
	return g_pkey_ok;
}

int
xtc_pkey_alloc(int *out_key)
{
	int k;
	if (out_key == NULL)
		return XTC_E_INVAL;
	if (!xtc_pkey_supported())
		return XTC_E_NOSYS;   /* keys unusable here (WRPKRU traps or no PKU) */
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
