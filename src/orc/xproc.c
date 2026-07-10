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

#include <sys/wait.h>
#include <string.h>

#if !defined(_WIN32)

struct xtc_xproc {
	xtc_loop_t   *loop;
	xtc_osproc_t *os;          /* the fork'd child + control socket */
	int           ctrl_fd;     /* parent end (owned by os) */
	xtc_pid_t     shadow;      /* local proc mirroring the child's fate */
	int           have_shadow;
};

/* ---- child side ---------------------------------------------------- */

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

#else /* _WIN32 -- fork has no Windows equivalent yet */

int
xtc_xspawn(xtc_loop_t *loop, const char *name, xtc_xproc_root_fn root_fn,
           const void *arg, size_t arg_len, xtc_xproc_t **out)
{
	(void)loop; (void)name; (void)root_fn; (void)arg; (void)arg_len;
	if (out) *out = NULL;
	return XTC_E_NOSYS;
}
void xtc_xproc_destroy(xtc_xproc_t *p) { (void)p; }
long xtc_xproc_os_pid(const xtc_xproc_t *p) { (void)p; return -1; }
int  xtc_xsend(xtc_xproc_t *p, const void *m, size_t l)
{ (void)p; (void)m; (void)l; return XTC_E_NOSYS; }
int  xtc_xmonitor(xtc_xproc_t *p, uint64_t *r)
{ (void)p; (void)r; return XTC_E_NOSYS; }
int  xtc_xproc_child_main(int fd, xtc_xproc_root_fn fn, void *arg)
{ (void)fd; (void)fn; (void)arg; return XTC_E_NOSYS; }

#endif /* _WIN32 */
