/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_backtrace.c
 *	Native stack backtrace, configure-selected backend.
 *
 *	The backend is chosen in this priority order:
 *
 *	1. execinfo (XTC_HAVE_EXECINFO): glibc, macOS, and the BSDs ship
 *	   <execinfo.h> with backtrace()/backtrace_symbols_fd().  The _fd
 *	   variant does not allocate and is async-signal-safe, so the emit
 *	   path is usable from a crash handler.  Produces SYMBOLIZED frames
 *	   (the loader+dladdr machinery inside libc resolves names).
 *
 *	2. builtin unwind (XTC_HAVE_BUILTIN_UNWIND): the compiler's own
 *	   _Unwind_Backtrace from <unwind.h> (the C++ EH personality ABI,
 *	   provided by libgcc_s / LLVM's unwinder and auto-linked -- no
 *	   extra -l flag).  This walks the calling thread with the same
 *	   unwinder execinfo's backtrace() uses internally, but needs no
 *	   <execinfo.h>, so it covers libc's that lack execinfo (notably
 *	   musl) WITHOUT a third-party libunwind dependency.  Frames are
 *	   symbolized best-effort with dladdr() (XTC_HAVE_DLADDR), exactly
 *	   as the libunwind backend.  This sits ABOVE libunwind in priority
 *	   so libunwind becomes opt-in (--with-unwind=libunwind).
 *
 *	3. libunwind (XTC_HAVE_LIBUNWIND): an explicit opt-in fallback
 *	   (--with-unwind=libunwind) for the rare target whose toolchain
 *	   ships neither execinfo nor a working _Unwind_Backtrace.  __os_-
 *	   backtrace walks the calling thread with unw_step() and records
 *	   the instruction pointers.  __os_backtrace_emit symbolizes each
 *	   address best-effort with dladdr() when XTC_HAVE_DLADDR is set,
 *	   falling back to a bare hex address otherwise.  Frame WALKING and
 *	   ADDRESS emission are async-signal-safe (no allocation, write(2)
 *	   only); the dladdr name lookup is not guaranteed signal-safe but
 *	   is only a best-effort enrichment.
 *
 *	4. DbgHelp (_WIN32): CaptureStackBackTrace fills the frame array and
 *	   SymFromAddr resolves names against the loaded modules.  COMPILED
 *	   BUT NOT RUNTIME-VERIFIED on this porting host -- there is no
 *	   Windows machine in the build/CI matrix that exercises it.  The
 *	   code was written and reviewed against the Win32 DbgHelp API
 *	   documentation; it mirrors the untested-but-reviewed status of
 *	   src/io/io_aix.c.  DbgHelp's Sym* family is single-threaded and
 *	   NOT async-signal-safe, so the emit path serializes with a lock
 *	   and is suitable for the panic/abort path, not an arbitrary
 *	   in-flight signal.
 *
 *	4. Stub: any platform with none of the above gets a backtrace of
 *	   length 0.  The dump facility degrades to "no C stack, but full
 *	   proc/loop/mailbox state" rather than failing.
 *
 *	See src/inc/os_backtrace.h and docs/guide/debugging.md for the
 *	per-platform symbolization matrix.
 */

/*
 * dladdr(3) and its Dl_info struct are exposed only under _GNU_SOURCE on
 * glibc/musl; define it before any header so the libunwind backend can
 * symbolize.  Harmless on backends that do not use dladdr.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "xtc_int.h"

#include "os_backtrace.h"

#if defined(XTC_HAVE_EXECINFO)

#include <execinfo.h>
#include <unistd.h>

#if defined(__APPLE__)
/*
 * macOS/arm64 (and x86-64) fiber-stack-safe backtrace.
 *
 * The system backtrace() follows the frame-pointer chain and, when it is
 * called from inside a libxtc fiber (a small, guard-paged mmap stack --
 * e.g. xtc_dump() from a live proc), can run PAST the fiber stack top
 * into unmapped memory and SIGBUS.  (Linux/BSD execinfo terminate
 * cleanly on the same stacks, so only Darwin needs this.)  We instead
 * walk the chain ourselves and never dereference outside the mapped VM
 * region that holds the current stack -- mach_vm_region queries the
 * task's VM map, not the memory, so bounding the walk this way cannot
 * itself fault.  No coroutine-layer coupling: the active stack (fiber
 * mmap or OS-thread stack) is discovered at call time from its own SP.
 */
#include <mach/mach.h>
#include <mach/mach_vm.h>

static int
apple_stack_region(uintptr_t addr, uintptr_t *lo, uintptr_t *hi)
{
	mach_vm_address_t a = (mach_vm_address_t)addr;
	mach_vm_size_t sz = 0;
	vm_region_basic_info_data_64_t info;
	mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t obj = MACH_PORT_NULL;

	if (mach_vm_region(mach_task_self(), &a, &sz,
	    VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt,
	    &obj) != KERN_SUCCESS)
		return 0;
	/* mach_vm_region returns the region at or above addr; require it to
	 * actually contain addr (true for any live stack slot). */
	if ((uintptr_t)a > addr)
		return 0;
	*lo = (uintptr_t)a;
	*hi = (uintptr_t)a + (uintptr_t)sz;
	return 1;
}

/* PUBLIC: int __os_backtrace __P((void **, int)); */
int
__os_backtrace(void **frames, int max)
{
	uintptr_t fp, lo, hi;
	int n = 0;

	if (frames == NULL || max <= 0)
		return 0;
	fp = (uintptr_t)__builtin_frame_address(0);
	if (!apple_stack_region(fp, &lo, &hi)) {
		/*
		 * Could not bound the current stack's VM region.  We must
		 * NOT fall back to the system backtrace() here: it walks the
		 * FP chain unbounded and, on a guard-paged fiber stack, can
		 * run past the top into unmapped memory and SIGBUS -- the
		 * exact fault this walker exists to prevent.  A missing
		 * region is rare (mach_vm_region essentially always resolves
		 * a live SP), so returning no frames (the dump prints
		 * "backtrace unavailable") is the safe, non-faulting choice.
		 */
		return 0;
	}
	/*
	 * arm64 and x86-64 both store the frame record as
	 * {saved FP, return addr} at [fp]; walk outward (ascending
	 * addresses) recording each return address.  Stop the moment the
	 * next slot would leave the mapped region, is misaligned, or does
	 * not ascend -- so an over-walk past a guard-paged fiber stack top
	 * can never touch an unmapped page.
	 */
	while (n < max && fp >= lo &&
	    fp + 2 * sizeof(uintptr_t) <= hi &&
	    (fp & (sizeof(uintptr_t) - 1)) == 0) {
		uintptr_t next = ((uintptr_t *)fp)[0];
		uintptr_t ret  = ((uintptr_t *)fp)[1];

		if (ret != 0)
			frames[n++] = (void *)ret;
		if (next <= fp)
			break;
		fp = next;
	}
	return n;
}
#else /* non-Apple execinfo (Linux glibc, the BSDs) */
/* PUBLIC: int __os_backtrace __P((void **, int)); */
int
__os_backtrace(void **frames, int max)
{
	if (frames == NULL || max <= 0)
		return 0;
	return backtrace(frames, max);
}
#endif /* __APPLE__ */

/* PUBLIC: void __os_backtrace_emit __P((int, void *const *, int)); */
void
__os_backtrace_emit(int fd, void *const *frames, int n)
{
	if (frames == NULL || n <= 0)
		return;
	/* backtrace_symbols_fd writes directly to fd with no malloc:
	 * async-signal-safe, unlike backtrace_symbols. */
	backtrace_symbols_fd((void *const *)frames, n, fd);
}

/* PUBLIC: int __os_backtrace_supported __P((void)); */
int
__os_backtrace_supported(void)
{
	return 1;
}

#elif defined(XTC_HAVE_BUILTIN_UNWIND) || defined(XTC_HAVE_LIBUNWIND)

#if defined(XTC_HAVE_LIBUNWIND)
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#else
#include <unwind.h>
#endif
#include <unistd.h>
#include <string.h>

#if defined(XTC_HAVE_DLADDR)
#include <dlfcn.h>
#endif

/*
 * Async-signal-safe helpers.  We avoid stdio entirely on the emit path:
 * a fixed-width hex/decimal formatter feeding write(2).
 */
static void
bt_write(int fd, const char *s, size_t len)
{
	while (len > 0) {
		ssize_t w = write(fd, s, len);
		if (w <= 0)
			return;
		s += (size_t)w;
		len -= (size_t)w;
	}
}

/* Append the lowercase hex of `v` (with "0x" prefix) into buf at *off. */
static void
bt_hex(char *buf, size_t cap, size_t *off, uintptr_t v)
{
	static const char hexd[] = "0123456789abcdef";
	char tmp[2 + 2 * sizeof(uintptr_t)];
	int i = (int)sizeof(tmp);

	do {
		tmp[--i] = hexd[v & 0xf];
		v >>= 4;
	} while (v != 0 && i > 2);
	tmp[--i] = 'x';
	tmp[--i] = '0';
	while (i < (int)sizeof(tmp) && *off + 1 < cap)
		buf[(*off)++] = tmp[i++];
}

/* Append the decimal of `v` into buf at *off. */
static void
bt_dec(char *buf, size_t cap, size_t *off, unsigned long v)
{
	char tmp[3 * sizeof(unsigned long) + 1];
	int i = (int)sizeof(tmp);

	do {
		tmp[--i] = (char)('0' + (v % 10));
		v /= 10;
	} while (v != 0 && i > 0);
	while (i < (int)sizeof(tmp) && *off + 1 < cap)
		buf[(*off)++] = tmp[i++];
}

static void
bt_str(char *buf, size_t cap, size_t *off, const char *s)
{
	while (*s != '\0' && *off + 1 < cap)
		buf[(*off)++] = *s++;
}

/* PUBLIC: int __os_backtrace __P((void **, int)); */
#if defined(XTC_HAVE_LIBUNWIND)
int
__os_backtrace(void **frames, int max)
{
	unw_context_t uc;
	unw_cursor_t cur;
	int n = 0;

	if (frames == NULL || max <= 0)
		return 0;

	if (unw_getcontext(&uc) != 0)
		return 0;
	if (unw_init_local(&cur, &uc) != 0)
		return 0;

	/*
	 * Walk outward from the current frame.  unw_get_reg(UNW_REG_IP)
	 * yields the instruction pointer of each frame; we record it as a
	 * void * so it round-trips through the same array execinfo uses.
	 */
	while (n < max) {
		unw_word_t ip = 0;
		if (unw_get_reg(&cur, UNW_REG_IP, &ip) != 0)
			break;
		frames[n++] = (void *)(uintptr_t)ip;
		if (unw_step(&cur) <= 0)
			break;
	}
	return n;
}
#else /* XTC_HAVE_BUILTIN_UNWIND */
/*
 * The compiler/runtime's own unwinder.  _Unwind_Backtrace invokes our
 * trace callback once per frame from innermost outward; _Unwind_GetIP
 * yields each frame's instruction pointer.  This is the same EH-ABI
 * unwinder libgcc_s / LLVM provide and that glibc's backtrace() uses
 * internally -- but it needs no <execinfo.h>, so it works on musl with
 * no third-party libunwind.  The walk allocates nothing and is async-
 * signal-safe.
 */
struct bt_ctx {
	void **frames;
	int    max;
	int    n;
};

static _Unwind_Reason_Code
bt_trace_cb(struct _Unwind_Context *uctx, void *arg)
{
	struct bt_ctx *c = arg;
	uintptr_t ip;

	if (c->n >= c->max)
		return _URC_END_OF_STACK;
	ip = (uintptr_t)_Unwind_GetIP(uctx);
	if (ip == 0)
		return _URC_END_OF_STACK;
	c->frames[c->n++] = (void *)ip;
	return _URC_NO_REASON;
}

int
__os_backtrace(void **frames, int max)
{
	struct bt_ctx c;

	if (frames == NULL || max <= 0)
		return 0;
	c.frames = frames;
	c.max = max;
	c.n = 0;
	(void)_Unwind_Backtrace(bt_trace_cb, &c);
	return c.n;
}
#endif /* XTC_HAVE_LIBUNWIND vs builtin */

/* PUBLIC: void __os_backtrace_emit __P((int, void *const *, int)); */
void
__os_backtrace_emit(int fd, void *const *frames, int n)
{
	int i;

	if (frames == NULL || n <= 0)
		return;

	for (i = 0; i < n; i++) {
		char line[256];
		size_t off = 0;
		uintptr_t ip = (uintptr_t)frames[i];

		/* "#<idx> 0x<ip>" */
		bt_str(line, sizeof(line), &off, "#");
		bt_dec(line, sizeof(line), &off, (unsigned long)i);
		bt_str(line, sizeof(line), &off, " ");
		bt_hex(line, sizeof(line), &off, ip);

#if defined(XTC_HAVE_DLADDR)
		{
			Dl_info info;
			/*
			 * dladdr() is best-effort and not formally async-
			 * signal-safe; we use it only to enrich the line and
			 * always have the raw address above as the fallback.
			 */
			if (dladdr((void *)ip, &info) != 0) {
				if (info.dli_sname != NULL) {
					bt_str(line, sizeof(line), &off,
					    " <");
					bt_str(line, sizeof(line), &off,
					    info.dli_sname);
					if (info.dli_saddr != NULL) {
						bt_str(line, sizeof(line),
						    &off, "+");
						bt_hex(line, sizeof(line),
						    &off,
						    ip - (uintptr_t)
						    info.dli_saddr);
					}
					bt_str(line, sizeof(line), &off, ">");
				}
				if (info.dli_fname != NULL) {
					bt_str(line, sizeof(line), &off,
					    " (");
					bt_str(line, sizeof(line), &off,
					    info.dli_fname);
					bt_str(line, sizeof(line), &off, ")");
				}
			}
		}
#endif
		if (off + 1 < sizeof(line))
			line[off++] = '\n';
		bt_write(fd, line, off);
	}
}

/* PUBLIC: int __os_backtrace_supported __P((void)); */
int
__os_backtrace_supported(void)
{
	return 1;
}

#elif defined(_WIN32)

/*
 * Windows DbgHelp backend.  COMPILED BUT NOT RUNTIME-VERIFIED on the
 * porting host (no Windows machine exercises it in the build/CI matrix).
 * Reviewed against the Win32 DbgHelp API docs; matches the untested-but-
 * reviewed status documented for src/io/io_aix.c.
 *
 * CaptureStackBackTrace captures return addresses without symbol info;
 * SymFromAddr resolves each against the loaded modules.  The Sym* family
 * is single-threaded, so emit serializes through a process-wide critical
 * section and is intended for the panic/abort path.
 */

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>
#include <io.h>

static volatile LONG sym_init_done;
static CRITICAL_SECTION sym_lock;
static volatile LONG sym_lock_init;

static void
ensure_sym_lock(void)
{
	if (InterlockedCompareExchange(&sym_lock_init, 1, 0) == 0)
		InitializeCriticalSection(&sym_lock);
}

/* PUBLIC: int __os_backtrace __P((void **, int)); */
int
__os_backtrace(void **frames, int max)
{
	if (frames == NULL || max <= 0)
		return 0;
	/* CaptureStackBackTrace caps at a USHORT count. */
	if (max > 0xffff)
		max = 0xffff;
	return (int)CaptureStackBackTrace(0, (ULONG)max, frames, NULL);
}

/* PUBLIC: void __os_backtrace_emit __P((int, void *const *, int)); */
void
__os_backtrace_emit(int fd, void *const *frames, int n)
{
	HANDLE proc;
	int i;
	/*
	 * SYMBOL_INFO is variable-length: the resolved name is written into
	 * the bytes that follow the fixed header.  The union keeps the
	 * trailing-name storage correctly aligned for SYMBOL_INFO.
	 */
	union {
		SYMBOL_INFO si;
		char raw[sizeof(SYMBOL_INFO) + 256];
	} symbuf;
	SYMBOL_INFO *sym = &symbuf.si;

	if (frames == NULL || n <= 0)
		return;

	ensure_sym_lock();
	EnterCriticalSection(&sym_lock);

	proc = GetCurrentProcess();
	if (InterlockedCompareExchange(&sym_init_done, 1, 0) == 0)
		(void)SymInitialize(proc, NULL, TRUE);

	for (i = 0; i < n; i++) {
		char line[512];
		int len;
		DWORD64 addr = (DWORD64)(uintptr_t)frames[i];
		DWORD64 disp = 0;

		memset(&symbuf, 0, sizeof(symbuf));
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;

		if (SymFromAddr(proc, addr, &disp, sym))
			len = snprintf(line, sizeof(line),
			    "#%d 0x%llx <%s+0x%llx>\n", i,
			    (unsigned long long)addr, sym->Name,
			    (unsigned long long)disp);
		else
			len = snprintf(line, sizeof(line),
			    "#%d 0x%llx\n", i,
			    (unsigned long long)addr);
		if (len > 0)
			(void)_write(fd, line, (unsigned)len);
	}

	LeaveCriticalSection(&sym_lock);
}

/* PUBLIC: int __os_backtrace_supported __P((void)); */
int
__os_backtrace_supported(void)
{
	return 1;
}

#else /* stub */

/* PUBLIC: int __os_backtrace __P((void **, int)); */
int
__os_backtrace(void **frames, int max)
{
	(void)frames;
	(void)max;
	return 0;
}

/* PUBLIC: void __os_backtrace_emit __P((int, void *const *, int)); */
void
__os_backtrace_emit(int fd, void *const *frames, int n)
{
	(void)fd;
	(void)frames;
	(void)n;
}

/* PUBLIC: int __os_backtrace_supported __P((void)); */
int
__os_backtrace_supported(void)
{
	return 0;
}

#endif
