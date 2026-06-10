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
#include <signal.h>
#include <setjmp.h>
#include <string.h>
#include <pthread.h>

/*
 * Whether protection keys are actually usable is decided ONCE, by a
 * guarded probe.  CPUID OSPKE and a successful pkey_alloc() are both
 * necessary but NOT sufficient: a kernel can support the pkey syscalls
 * (and even report OSPKE) while the WRPKRU instruction that glibc's
 * pkey_set() executes in userspace still faults with SIGILL -- seen on
 * virtualized CI runners.  So we additionally execute a real pkey_set
 * under a temporary SIGILL handler: if WRPKRU traps we longjmp out and
 * report "unsupported", so callers never issue WRPKRU on a host where
 * it faults.  The probe runs at most once (pthread_once).
 */
static int            g_pkey_ok;
static pthread_once_t g_pkey_once = PTHREAD_ONCE_INIT;
static sigjmp_buf     g_pkey_jmp;
static volatile sig_atomic_t g_pkey_probing;

static void
pkey_sigill(int sig)
{
	(void)sig;
	if (g_pkey_probing)
		siglongjmp(g_pkey_jmp, 1);   /* WRPKRU trapped: not usable */
	_exit(128 + 4);                     /* a real SIGILL elsewhere */
}

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
	struct sigaction sa, old;
	int k;

	g_pkey_ok = 0;
	if (!x86_ospke())
		return;                       /* CR4.PKE clear: WRPKRU would trap */
	k = pkey_alloc(0, 0);
	if (k < 0)
		return;                       /* kernel without pkey support */

	/* OSPKE and the syscall agree; confirm WRPKRU itself does not trap. */
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = pkey_sigill;
	sa.sa_flags = SA_NODEFER;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGILL, &sa, &old) == 0) {
		g_pkey_probing = 1;
		if (sigsetjmp(g_pkey_jmp, 1) == 0) {
			(void)pkey_set(k, 0);        /* the WRPKRU path */
			g_pkey_ok = 1;              /* survived: usable */
		}
		g_pkey_probing = 0;
		(void)sigaction(SIGILL, &old, NULL);
	}
	(void)pkey_free(k);
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
