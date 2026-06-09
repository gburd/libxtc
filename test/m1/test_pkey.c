/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m1/test_pkey.c
 *	Memory-protection-key tier (isolation c).  Everywhere: the
 *	NOSYS contract when unsupported.  On Linux/x86 with PKU: a
 *	write to a write-disabled protected page faults (enforcement),
 *	checked in a forked child.
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_pkey.h"

#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))
#  include <signal.h>
#  include <sys/mman.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  define PKEY_HAVE_ENFORCE_TEST 1
#endif

static MunitResult
test_pkey(const MunitParameter p[], void *d)
{
	(void)p; (void)d;

	if (!xtc_pkey_supported()) {
		int k = -1;
		munit_assert_int(xtc_pkey_alloc(&k), ==, XTC_E_NOSYS);
		munit_assert_int(xtc_pkey_set_access(0, 1, 1), ==, XTC_E_NOSYS);
		munit_assert_int(xtc_pkey_free(0), ==, XTC_E_NOSYS);
		return MUNIT_OK;   /* portable NOSYS contract verified */
	}

#if defined(PKEY_HAVE_ENFORCE_TEST)
	{
		long pg = sysconf(_SC_PAGESIZE);
		size_t len = pg > 0 ? (size_t)pg : 4096;
		int key = -1, st = 0;
		pid_t pid;
		void *mem;

		munit_assert_int(xtc_pkey_alloc(&key), ==, XTC_OK);
		mem = mmap(NULL, len, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		munit_assert_ptr_not_equal(mem, MAP_FAILED);
		munit_assert_int(xtc_pkey_protect(mem, len, key), ==, XTC_OK);

		/* Full access: a write succeeds. */
		munit_assert_int(xtc_pkey_set_access(key, 1, 1), ==, XTC_OK);
		((volatile char *)mem)[0] = 1;

		/* Disable write for this key, then write in a child: must fault. */
		pid = fork();
		munit_assert_int(pid, >=, 0);
		if (pid == 0) {
			(void)xtc_pkey_set_access(key, 1, 0);  /* read-only */
			((volatile char *)mem)[0] = 2;         /* should SIGSEGV */
			_exit(0);                              /* no fault: fail */
		}
		munit_assert_int(waitpid(pid, &st, 0), ==, pid);
		munit_assert_true(WIFSIGNALED(st));
		munit_assert_int(WTERMSIG(st), ==, SIGSEGV);

		(void)munmap(mem, len);
		munit_assert_int(xtc_pkey_free(key), ==, XTC_OK);
	}
#endif
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/protect", test_pkey, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m1/pkey", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
