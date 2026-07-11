/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/xproc.c
 *	Cross-fork spawn / send / monitor over the xtc_osproc control
 *	socketpair.  See src/inc/xtc_xproc.h.
 *
 *	Design: fork via xtc_osproc (which already gives a pidfd-pollable
 *	child + a control socket + raw waitpid-status reaping).  The child
 *	runs xtc_xproc_child_main, which stands up a child runtime and
 *	pumps parent xtc_xsend payloads into the root proc's mailbox.
 *
 *	The parent mirrors the child's fate into a LOCAL "shadow" proc: a
 *	relay fiber xtc_osproc_wait()s the child, decodes its status, and
 *	exits the shadow proc with the matching reason -- so a caller just
 *	xtc_monitors the shadow proc and receives an ordinary, fully
 *	classified xtc DOWN (EXIT / SIGNAL / NOCONNECTION).  This reuses
 *	all existing link/monitor plumbing rather than inventing a second
 *	DOWN path.
 */

#include "xtc_int.h"
#include "xtc_xproc.h"
#include "xtc_osproc.h"
#include "xtc_net.h"
#include "xtc_proc.h"
#include "xtc_preempt.h"   /* __xtc_mtx_lock/unlock for the entry registry */

#include <string.h>
#include <pthread.h>

/* ---- portable child-entry registry (both platforms) ----------------
 *
 * A raw function pointer cannot survive process creation on Windows (a
 * child is a fresh image), so a child root function is addressed by a
 * registered NAME that the identical binary resolves the same way in
 * parent and child.  xtc_xspawn_entry uses this; POSIX xtc_xspawn keeps
 * the direct-pointer path (fork preserves the address space).
 */
#define XPROC_MAX_ENTRIES 64
struct xproc_entry { const char *name; xtc_xproc_root_fn fn; };
static struct xproc_entry  __xproc_entries[XPROC_MAX_ENTRIES];
static int                 __xproc_n_entries;
static pthread_mutex_t      __xproc_entry_lock = PTHREAD_MUTEX_INITIALIZER;

int
xtc_xproc_register_entry(const char *name, xtc_xproc_root_fn fn)
{
	int i, rc = XTC_OK;
	if (name == NULL || fn == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&__xproc_entry_lock);
	for (i = 0; i < __xproc_n_entries; i++) {
		if (strcmp(__xproc_entries[i].name, name) == 0) {
			__xproc_entries[i].fn = fn;   /* replace */
			goto out;
		}
	}
	if (__xproc_n_entries == XPROC_MAX_ENTRIES) { rc = XTC_E_RESOURCE; goto out; }
	__xproc_entries[__xproc_n_entries].name = name;
	__xproc_entries[__xproc_n_entries].fn = fn;
	__xproc_n_entries++;
out:
	(void)__xtc_mtx_unlock(&__xproc_entry_lock);
	return rc;
}

/* Resolve a registered entry name to its function (NULL if unknown). */
static xtc_xproc_root_fn
__xproc_lookup_entry(const char *name)
{
	xtc_xproc_root_fn fn = NULL;
	int i;
	if (name == NULL) return NULL;
	(void)__xtc_mtx_lock(&__xproc_entry_lock);
	for (i = 0; i < __xproc_n_entries; i++)
		if (strcmp(__xproc_entries[i].name, name) == 0) {
			fn = __xproc_entries[i].fn;
			break;
		}
	(void)__xtc_mtx_unlock(&__xproc_entry_lock);
	return fn;
}

/* ---- child side (shared by both platforms) -------------------------
 *
 * The child-runtime pump procs use only portable primitives (xtc_recv,
 * xtc_send, xtc_net_recv_frame, xtc_monitor, xtc_down_decode_ex), so
 * they live above the platform split and are used by both the POSIX and
 * Windows xtc_xproc_child_main. */

/* Delivered to the child root proc as its arg: the copied spawn arg. */
struct child_root_ctx {
	xtc_xproc_root_fn root_fn;
	void             *arg;      /* copy of the parent's arg bytes */
};

/* The child's root proc: run the user's root_fn with its arg.  Messages
 * the parent xtc_xsends are delivered to THIS proc's mailbox by the
 * relay below, so root_fn just xtc_recv()s them. */
static void
child_root_proc(void *a)
{
	struct child_root_ctx *c = a;
	c->root_fn(c->arg);
}

/* Child pump proc: forward each parent frame into the root proc's
 * mailbox, and stop when the root proc goes DOWN (so the child can
 * exit).  Runs as a fiber, so recv_frame parks rather than blocking the
 * OS thread. */
struct child_pump_ctx {
	int       ctrl_fd;
	xtc_pid_t root;
	int      *exit_code;   /* written with the root's exit reason */
};

static void
child_pump_proc(void *a)
{
	struct child_pump_ctx *pp = a;
	uint64_t ref = 0;

	/* Monitor the root proc so we learn its exit reason and when to
	 * stop pumping. */
	(void)xtc_monitor(pp->root, &ref);

	for (;;) {
		void *frame = NULL;
		size_t flen = 0;
		int frc;

		/* Wait up to a short slice for a parent frame; a timeout lets
		 * us re-check whether the root proc has exited (its DOWN also
		 * lands in our mailbox, but recv_frame only reads the fd). */
		frc = xtc_net_recv_frame(pp->ctrl_fd, &frame, &flen, 0,
		    50LL * 1000 * 1000);
		if (frc == XTC_OK) {
			if (frame != NULL && flen > 0)
				(void)xtc_send(pp->root, frame, flen);
			if (frame != NULL)
				xtc_free(frame);
			continue;
		}
		if (frc == XTC_E_AGAIN) {
			/* No frame this slice.  Did the root proc exit?  Drain
			 * any DOWN sitting in our mailbox. */
			void *m = NULL; size_t mn = 0;
			if (xtc_recv(&m, &mn, 0) == XTC_OK) {
				xtc_down_info_t di;
				if (xtc_down_decode_ex(m, mn, &di) == XTC_OK &&
				    pp->exit_code != NULL)
					*pp->exit_code = di.reason;
				if (m) xtc_free(m);
				return;   /* root gone -> stop pumping */
			}
			continue;
		}
		/* Channel closed (parent went away): stop. */
		if (frame != NULL) xtc_free(frame);
		return;
	}
}

#if !defined(_WIN32)

#include <sys/wait.h>

struct xtc_xproc {
	xtc_loop_t   *loop;
	xtc_osproc_t *os;          /* the fork'd child + control socket */
	int           ctrl_fd;     /* parent end (owned by os) */
	xtc_pid_t     shadow;      /* local proc mirroring the child's fate */
	int           have_shadow;
};


/* Runs in the fork'd child (from the osproc fn trampoline).  Stands up a
 * child runtime, spawns the root proc + a pump proc that forwards parent
 * frames into the root's mailbox and watches for the root's exit, then
 * runs the loop until both finish.  Exits with the root proc's reason. */
int
xtc_xproc_child_main(int ctrl_fd, xtc_xproc_root_fn root_fn, void *arg)
{
	xtc_loop_t *loop = NULL;
	struct child_root_ctx rctx;
	struct child_pump_ctx pctx;
	xtc_pid_t root;
	int exit_code = 0;

	if (root_fn == NULL) return 2;
	if (xtc_loop_init(&loop) != XTC_OK) return 3;

	/* The control fd must be non-blocking so recv_frame parks the pump
	 * fiber instead of blocking the child's loop thread. */
	(void)xtc_net_setnonblock(ctrl_fd);

	rctx.root_fn = root_fn;
	rctx.arg = arg;
	if (xtc_proc_spawn(loop, child_root_proc, &rctx, NULL, &root) != XTC_OK) {
		(void)xtc_loop_fini(loop);
		return 4;
	}
	pctx.ctrl_fd = ctrl_fd;
	pctx.root = root;
	pctx.exit_code = &exit_code;
	if (xtc_proc_spawn(loop, child_pump_proc, &pctx, NULL, NULL) != XTC_OK) {
		(void)xtc_loop_fini(loop);
		return 5;
	}

	/* Run until both the root and the pump finish (loop goes idle). */
	(void)xtc_loop_run(loop);
	(void)xtc_loop_fini(loop);
	return exit_code;
}

/* POSIX no-op: a fork'd child is not re-exec'd, so there is no sentinel
 * argv to detect.  Present so the symbol exists on every platform. */
int
xtc_xproc_win_child_maybe(int argc, char **argv)
{
	(void)argc; (void)argv;
	return 0;
}

/* osproc fn trampoline: what the child runs immediately after fork.
 * `arg` is the child_root_ctx pointer we stashed pre-fork (fork copies
 * the address space, so the pointer is valid in the child). */
struct child_spawn_ctx {
	xtc_xproc_root_fn root_fn;
	void             *arg;       /* copied arg bytes (or NULL) */
};

static int
child_fn(int ctrl_fd, void *a)
{
	struct child_spawn_ctx *c = a;
	return xtc_xproc_child_main(ctrl_fd, c->root_fn, c->arg);
}

/* ---- parent side --------------------------------------------------- */

/* The shadow proc: wait for the child, decode its status, and exit with
 * the matching reason so a monitor of THIS proc sees the child's fate. */
struct shadow_ctx {
	xtc_osproc_t *os;
	int           ctrl_fd;
};

static void
shadow_proc(void *a)
{
	struct shadow_ctx *s = a;
	xtc_osproc_t *os = s->os;
	int status = 0;
	int reason;

	/* Park until the child exits (never blocks the loop thread). */
	if (xtc_osproc_wait(os, &status, -1) != XTC_OK) {
		__os_free(s);
		xtc_exit_self(XTC_DOWN_KIND_NOCONNECTION);
		return;
	}
	/* Decode the raw waitpid status into an exit reason.  exit(0) -> 0;
	 * exit(code) -> code; signal N -> N (the same convention the
	 * intra-process fault path uses, so a monitor reading the legacy
	 * `reason` sees the signal number). */
	if (WIFSIGNALED(status))
		reason = WTERMSIG(status);
	else if (WIFEXITED(status))
		reason = WEXITSTATUS(status);
	else
		reason = XTC_DOWN_KIND_NOCONNECTION;
	__os_free(s);          /* freed here; xtc_exit_self longjmps away */
	xtc_exit_self(reason);
}

int
xtc_xspawn(xtc_loop_t *loop, const char *name, xtc_xproc_root_fn root_fn,
           const void *arg, size_t arg_len, xtc_xproc_t **out)
{
	struct xtc_xproc *p = NULL;
	struct child_spawn_ctx *cctx = NULL;
	xtc_osproc_opts_t oo;
	void *arg_copy = NULL;
	int rc;

	if (loop == NULL || root_fn == NULL || out == NULL)
		return XTC_E_INVAL;
	*out = NULL;

	if ((rc = __os_calloc(1, sizeof *p, (void **)&p)) != XTC_OK)
		return rc;
	if ((rc = __os_calloc(1, sizeof *cctx, (void **)&cctx)) != XTC_OK)
		goto fail_p;

	/* Copy the arg bytes so the child (post-fork) has its own copy at a
	 * stable address (fork preserves the address space, so a heap copy
	 * made before fork is valid in the child). */
	if (arg_len > 0 && arg != NULL) {
		if ((rc = __os_malloc(arg_len, &arg_copy)) != XTC_OK)
			goto fail_c;
		memcpy(arg_copy, arg, arg_len);
	}
	cctx->root_fn = root_fn;
	cctx->arg = arg_copy;

	memset(&oo, 0, sizeof oo);
	oo.name = name;
	oo.fn = child_fn;
	oo.arg = cctx;
	oo.ctrl_socket = 1;

	if ((rc = xtc_osproc_spawn(&oo, &p->os)) != XTC_OK)
		goto fail_arg;

	/* The child inherited its own copy of cctx/arg_copy via fork; the
	 * parent no longer needs them. */
	if (arg_copy != NULL) __os_free(arg_copy);
	__os_free(cctx);

	p->loop = loop;
	p->ctrl_fd = xtc_osproc_ctrl_fd(p->os);
	*out = p;
	return XTC_OK;

fail_arg:
	if (arg_copy != NULL) __os_free(arg_copy);
fail_c:
	__os_free(cctx);
fail_p:
	__os_free(p);
	return rc;
}

/* POSIX xtc_xspawn_entry: resolve the registered entry name to its
 * function and delegate to the fork path.  (On Windows this is a
 * distinct re-exec implementation; see the _WIN32 block.) */
int
xtc_xspawn_entry(xtc_loop_t *loop, const char *name, const char *entry,
                 const void *arg, size_t arg_len, xtc_xproc_t **out)
{
	xtc_xproc_root_fn fn = __xproc_lookup_entry(entry);
	if (out != NULL) *out = NULL;
	if (loop == NULL || entry == NULL || out == NULL) return XTC_E_INVAL;
	if (fn == NULL) return XTC_E_NOTFOUND;
	return xtc_xspawn(loop, name, fn, arg, arg_len, out);
}

void
xtc_xproc_destroy(xtc_xproc_t *p)
{
	if (p == NULL) return;
	if (p->os != NULL)
		xtc_osproc_destroy(p->os);   /* signals + reaps if running */
	__os_free(p);
}

long
xtc_xproc_os_pid(const xtc_xproc_t *p)
{
	if (p == NULL || p->os == NULL) return -1;
	return xtc_osproc_pid(p->os);
}

int
xtc_xsend(xtc_xproc_t *p, const void *msg, size_t len)
{
	if (p == NULL || p->ctrl_fd < 0) return XTC_E_INVAL;
	if (len > 0 && msg == NULL) return XTC_E_INVAL;
	return xtc_net_send_frame(p->ctrl_fd, msg, len);
}

int
xtc_xmonitor(xtc_xproc_t *p, uint64_t *out_ref)
{
	struct shadow_ctx *s = NULL;
	int rc;

	if (p == NULL || p->os == NULL) return XTC_E_INVAL;
	if (p->have_shadow)
		return xtc_monitor(p->shadow, out_ref);   /* re-monitor */

	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK)
		return rc;
	s->os = p->os;
	s->ctrl_fd = p->ctrl_fd;
	/* Spawn the shadow; monitor it BEFORE it can exit (spawn-then-
	 * monitor on the same loop is race-free: the shadow cannot run
	 * until the caller yields).  The shadow frees `s` itself before it
	 * exits (it runs exactly once). */
	if ((rc = xtc_proc_spawn(p->loop, shadow_proc, s, NULL,
	    &p->shadow)) != XTC_OK) {
		__os_free(s);
		return rc;
	}
	p->have_shadow = 1;
	return xtc_monitor(p->shadow, out_ref);
}

#else /* _WIN32 -- no fork; a real re-exec + loopback-control port. */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>

/*
 * Windows cross-process spawn/monitor.  There is no fork(): a child is
 * a fresh CreateProcess'd image, so the child body cannot be a raw
 * function pointer -- it must be a REGISTERED ENTRY NAME the identical
 * binary resolves the same way (xtc_xproc_register_entry).  Hence
 * xtc_xspawn (pointer form) declines with XTC_E_NOSYS on Windows; the
 * portable xtc_xspawn_entry works here.
 *
 * Contract mirrors POSIX: the parent gets an xtc_xproc handle it can
 * xtc_xsend to and xtc_xmonitor; a child exit / crash surfaces as a
 * normal xtc DOWN whose reason is the exit code, or a signal number
 * mapped from the unhandled-exception NTSTATUS (Cygwin's table), or
 * XTC_DOWN_KIND_NOCONNECTION if the control channel died first.
 *
 * Control channel: a hardened loopback-TCP socket pair (Winsock has no
 * socketpair) -- listen on 127.0.0.1:0, the child connects, and both
 * sides exchange a random nonce so a local process cannot hijack the
 * ephemeral port (the ZeroMQ make_fdpair technique).
 *
 * Child re-entry: the re-exec'd image detects the sentinel argv
 * ("--xtc-xproc-child <entry> <port> <nonce>"), connects the control
 * socket, verifies the nonce, receives the copied arg, looks up the
 * entry, and runs xtc_xproc_child_main.  The embedder wires this by
 * calling xtc_xproc_win_child_maybe() early in main() (a no-op unless
 * the sentinel is present).
 */

struct xtc_xproc {
	xtc_loop_t   *loop;
	HANDLE        proc;        /* child process handle */
	HANDLE        wait;        /* RegisterWaitForSingleObject handle */
	SOCKET        ctrl;        /* parent end of the loopback control pair */
	int           ctrl_fd;     /* ctrl as an int for xtc_net_* framing */
	long          os_pid;
	DWORD         exit_code;
	_Atomic int   exited;      /* set by the wait callback */
	xtc_pid_t     shadow;
	int           have_shadow;
};

/* NTSTATUS unhandled-exception exit code -> POSIX signal number
 * (Cygwin winsup/cygwin/exceptions.cc mapping; the de-facto reference). */
static int
__ntstatus_to_signal(DWORD code)
{
	switch (code) {
	case 0xC0000005: return 11;   /* ACCESS_VIOLATION      -> SIGSEGV */
	case 0xC00000FD: return 11;   /* STACK_OVERFLOW        -> SIGSEGV */
	case 0xC000001D: return 4;    /* ILLEGAL_INSTRUCTION   -> SIGILL  */
	case 0xC0000096: return 4;    /* PRIVILEGED_INSTRUCTION-> SIGILL  */
	case 0xC0000094: return 8;    /* INTEGER_DIVIDE_BY_ZERO-> SIGFPE  */
	case 0xC000008E: return 8;    /* FLT_DIVIDE_BY_ZERO    -> SIGFPE  */
	case 0xC0000090: return 8;    /* FLT_INVALID_OPERATION -> SIGFPE  */
	case 0xC000008C: return 11;   /* ARRAY_BOUNDS_EXCEEDED -> SIGSEGV */
	case 0x80000003: return 5;    /* BREAKPOINT            -> SIGTRAP */
	case 0xC000013A: return 2;    /* CONTROL_C_EXIT        -> SIGINT  */
	default:         return 0;    /* not a recognized fault */
	}
}

/* One-time Winsock init (idempotent; the embedder may also have done it). */
static void
__xproc_wsa_init(void)
{
	static _Atomic int done;
	int expected = 0;
	if (atomic_compare_exchange_strong(&done, &expected, 1)) {
		WSADATA w;
		(void)WSAStartup(MAKEWORD(2, 2), &w);
	}
}

/* Hardened loopback-TCP pair: returns a connected (listener-side) SOCKET
 * in *server and the ephemeral port + nonce the child must use in
 * *port/*nonce.  The child connects and echoes the nonce; the server
 * accepts and verifies it, defeating the port-hijack race. */
static int
__xproc_listen(SOCKET *listener, unsigned short *port, uint32_t *nonce)
{
	SOCKET ls;
	struct sockaddr_in a;
	int alen = (int)sizeof a;
	__xproc_wsa_init();
	ls = socket(AF_INET, SOCK_STREAM, 0);
	if (ls == INVALID_SOCKET) return XTC_E_IO;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = 0;
	if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 ||
	    listen(ls, 1) != 0 ||
	    getsockname(ls, (struct sockaddr *)&a, &alen) != 0) {
		closesocket(ls);
		return XTC_E_IO;
	}
	*listener = ls;
	*port = ntohs(a.sin_port);
	/* A per-spawn nonce.  xtc_rand is the seeded/portable RNG. */
	*nonce = (uint32_t)xtc_rand_u64();
	return XTC_OK;
}

/* Accept the child's connection and verify its nonce; return the
 * connected socket or INVALID_SOCKET. */
static SOCKET
__xproc_accept(SOCKET listener, uint32_t nonce)
{
	SOCKET c = accept(listener, NULL, NULL); /* XTC_BLOCKING_OK: one-time control-channel handshake, parent setup */
	uint32_t got = 0;
	if (c == INVALID_SOCKET) return INVALID_SOCKET;
	if (recv(c, (char *)&got, sizeof got, 0) != (int)sizeof got || /* XTC_BLOCKING_OK: nonce verify, setup */
	    got != nonce) {
		closesocket(c);
		return INVALID_SOCKET;   /* hijack / mismatch: reject */
	}
	return c;
}

/* Exit-wait thread-pool callback: latch the exit + code, then let the
 * shadow proc observe it. */
static VOID CALLBACK
__xproc_exit_cb(PVOID ctx, BOOLEAN timedout)
{
	struct xtc_xproc *p = ctx;
	DWORD code = 0;
	(void)timedout;
	if (GetExitCodeProcess(p->proc, &code))
		p->exit_code = code;
	atomic_store(&p->exited, 1);
}

/* Shadow proc: poll the exit latch (the wait callback runs on a pool
 * thread), then exit with the decoded reason so a local monitor sees a
 * normal DOWN.  Polls with a short fiber sleep -- correct and simple;
 * an IOCP-posted completion is the later optimization. */
struct win_shadow_ctx { struct xtc_xproc *p; };
static void
win_shadow_proc(void *a)
{
	struct win_shadow_ctx *s = a;
	struct xtc_xproc *p = s->p;
	int reason, sig;
	while (!atomic_load(&p->exited))
		xtc_proc_sleep(5LL * 1000 * 1000);   /* 5 ms */
	sig = __ntstatus_to_signal(p->exit_code);
	reason = (sig != 0) ? sig : (int)(p->exit_code & 0xff);
	__os_free(s);
	xtc_exit_self(reason);
}

int
xtc_xspawn(xtc_loop_t *loop, const char *name, xtc_xproc_root_fn root_fn,
           const void *arg, size_t arg_len, xtc_xproc_t **out)
{
	/* Pointer form cannot cross a fresh process image; use
	 * xtc_xspawn_entry on Windows. */
	(void)loop; (void)name; (void)root_fn; (void)arg; (void)arg_len;
	if (out) *out = NULL;
	return XTC_E_NOSYS;
}

int
xtc_xspawn_entry(xtc_loop_t *loop, const char *name, const char *entry,
                 const void *arg, size_t arg_len, xtc_xproc_t **out)
{
	struct xtc_xproc *p = NULL;
	SOCKET listener = INVALID_SOCKET, ctrl = INVALID_SOCKET;
	unsigned short port = 0;
	uint32_t nonce = 0;
	wchar_t exe[MAX_PATH];
	wchar_t cmd[MAX_PATH + 128];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	uint32_t alen;
	int rc;
	(void)name;

	if (loop == NULL || entry == NULL || out == NULL) return XTC_E_INVAL;
	*out = NULL;
	if (__xproc_lookup_entry(entry) == NULL) return XTC_E_NOTFOUND;
	if (arg_len > 0 && arg == NULL) return XTC_E_INVAL;

	if ((rc = __os_calloc(1, sizeof *p, (void **)&p)) != XTC_OK) return rc;
	p->proc = NULL; p->wait = NULL; p->ctrl = INVALID_SOCKET;
	p->ctrl_fd = -1; p->os_pid = -1;

	if ((rc = __xproc_listen(&listener, &port, &nonce)) != XTC_OK) {
		__os_free(p); return rc;
	}

	/* Re-exec this same binary with the child sentinel. */
	if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0) {
		closesocket(listener); __os_free(p); return XTC_E_IO;
	}
	_snwprintf(cmd, sizeof cmd / sizeof cmd[0],
	    L"\"%s\" --xtc-xproc-child %hs %u %u", exe, entry,
	    (unsigned)port, (unsigned)nonce);
	memset(&si, 0, sizeof si); si.cb = sizeof si;
	memset(&pi, 0, sizeof pi);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL,
	    &si, &pi)) {
		closesocket(listener); __os_free(p); return XTC_E_IO;
	}
	CloseHandle(pi.hThread);
	p->proc = pi.hProcess;
	p->os_pid = (long)pi.dwProcessId;

	/* Accept the child's control connection + verify the nonce. */
	ctrl = __xproc_accept(listener, nonce);
	closesocket(listener);
	if (ctrl == INVALID_SOCKET) {
		TerminateProcess(p->proc, 1);
		CloseHandle(p->proc); __os_free(p); return XTC_E_IO;
	}
	p->ctrl = ctrl;
	p->ctrl_fd = (int)ctrl;   /* xtc_net_* framing operates on this */

	/* Ship the copied arg to the child as the first frame. */
	alen = (uint32_t)arg_len;
	if (xtc_net_send_frame(p->ctrl_fd, arg, arg_len) != XTC_OK) {
		(void)alen;
		TerminateProcess(p->proc, 1);
		closesocket(p->ctrl); CloseHandle(p->proc);
		__os_free(p); return XTC_E_IO;
	}

	p->loop = loop;
	*out = p;
	return XTC_OK;
}

void
xtc_xproc_destroy(xtc_xproc_t *p)
{
	if (p == NULL) return;
	if (p->wait != NULL) UnregisterWaitEx(p->wait, INVALID_HANDLE_VALUE);
	if (p->proc != NULL) {
		DWORD code = 0;
		if (GetExitCodeProcess(p->proc, &code) && code == STILL_ACTIVE)
			TerminateProcess(p->proc, 1);
		CloseHandle(p->proc);
	}
	if (p->ctrl != INVALID_SOCKET) closesocket(p->ctrl);
	__os_free(p);
}

long
xtc_xproc_os_pid(const xtc_xproc_t *p)
{
	return p == NULL ? -1 : p->os_pid;
}

int
xtc_xsend(xtc_xproc_t *p, const void *msg, size_t len)
{
	if (p == NULL || p->ctrl_fd < 0) return XTC_E_INVAL;
	if (len > 0 && msg == NULL) return XTC_E_INVAL;
	return xtc_net_send_frame(p->ctrl_fd, msg, len);
}

int
xtc_xmonitor(xtc_xproc_t *p, uint64_t *out_ref)
{
	struct win_shadow_ctx *s = NULL;
	int rc;
	if (p == NULL || p->proc == NULL) return XTC_E_INVAL;
	if (p->have_shadow) return xtc_monitor(p->shadow, out_ref);

	/* Arm the exit wait (thread pool -> __xproc_exit_cb sets p->exited). */
	if (p->wait == NULL) {
		if (!RegisterWaitForSingleObject(&p->wait, p->proc,
		    __xproc_exit_cb, p, INFINITE, WT_EXECUTEONLYONCE))
			return XTC_E_IO;
	}
	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK) return rc;
	s->p = p;
	if ((rc = xtc_proc_spawn(p->loop, win_shadow_proc, s, NULL,
	    &p->shadow)) != XTC_OK) {
		__os_free(s); return rc;
	}
	p->have_shadow = 1;
	return xtc_monitor(p->shadow, out_ref);
}

int
xtc_xproc_register_entry(const char *name, xtc_xproc_root_fn fn);   /* portable, above */

/* Child-side: the re-exec'd image calls this early in main().  If the
 * sentinel argv is present, it connects the control socket, sends the
 * nonce, receives the arg frame, looks up the entry, runs
 * xtc_xproc_child_main, and _exit()s with its result -- never returning.
 * Otherwise it is a no-op and returns 0 (normal startup continues).
 * (Declared in xtc_xproc.h, like the rest of the xtc_xproc surface; no
 * PUBLIC marker here so s_include does not also emit it into orc_ext.h.)
 */
int
xtc_xproc_win_child_maybe(int argc, char **argv)
{
	const char *entry;
	unsigned port, nonce;
	SOCKET c;
	struct sockaddr_in a;
	xtc_xproc_root_fn fn;
	void *arg = NULL;
	size_t alen = 0;
	int i, r;

	for (i = 1; i + 3 < argc + 1 && i < argc; i++)
		if (strcmp(argv[i], "--xtc-xproc-child") == 0 && i + 3 < argc + 1)
			break;
	if (i >= argc || strcmp(argv[i], "--xtc-xproc-child") != 0 ||
	    i + 3 >= argc + 1 || argc - i < 4)
		return 0;   /* not a child launch */
	entry = argv[i + 1];
	port = (unsigned)strtoul(argv[i + 2], NULL, 10);
	nonce = (unsigned)strtoul(argv[i + 3], NULL, 10);

	__xproc_wsa_init();
	c = socket(AF_INET, SOCK_STREAM, 0);
	if (c == INVALID_SOCKET) return 3;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short)port);
	if (connect(c, (struct sockaddr *)&a, sizeof a) != 0) { /* XTC_BLOCKING_OK: child control-channel connect, setup */
		closesocket(c); return 3;
	}
	{ uint32_t n32 = (uint32_t)nonce;
	  (void)send(c, (const char *)&n32, sizeof n32, 0); } /* XTC_BLOCKING_OK: nonce send, setup */

	/* First frame is the parent's copied arg. */
	(void)xtc_net_recv_frame((int)c, &arg, &alen, 0, -1);
	fn = __xproc_lookup_entry(entry);
	if (fn == NULL) { closesocket(c); return 4; }

	r = xtc_xproc_child_main((int)c, fn, arg);
	if (arg != NULL) xtc_free(arg);
	closesocket(c);
	_exit(r & 0xff);
	return r;   /* unreached */
}

/* Child runtime pump (Windows).  The ctrl fd is a Winsock SOCKET cast
 * to int; xtc_net_* framing handles it.  Shares the child_root_proc /
 * child_pump_proc bodies with POSIX (hoisted above the platform split). */
int
xtc_xproc_child_main(int ctrl_fd, xtc_xproc_root_fn root_fn, void *arg)
{
	xtc_loop_t *loop = NULL;
	struct child_root_ctx rctx;
	struct child_pump_ctx pctx;
	xtc_pid_t root;
	int exit_code = 0;

	if (root_fn == NULL) return 2;
	if (xtc_loop_init(&loop) != XTC_OK) return 3;
	(void)xtc_net_setnonblock(ctrl_fd);

	rctx.root_fn = root_fn;
	rctx.arg = arg;
	if (xtc_proc_spawn(loop, child_root_proc, &rctx, NULL, &root) != XTC_OK) {
		(void)xtc_loop_fini(loop);
		return 4;
	}
	pctx.ctrl_fd = ctrl_fd;
	pctx.root = root;
	pctx.exit_code = &exit_code;
	if (xtc_proc_spawn(loop, child_pump_proc, &pctx, NULL, NULL) != XTC_OK) {
		(void)xtc_loop_fini(loop);
		return 5;
	}
	(void)xtc_loop_run(loop);
	(void)xtc_loop_fini(loop);
	return exit_code;
}

#endif /* _WIN32 */
