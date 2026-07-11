/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_xproc.h
 *	Cross-fork spawn / send / link / monitor: extend the Erlang-style
 *	process relations across a fork() boundary, not just between fibers
 *	inside one OS process.  This is the SINGLE-HOST subset of the
 *	distributed design -- a stepping stone toward --enable-dist, built
 *	over a local socketpair rather than TCP.
 *
 *	A parent xtc_xspawn()s a child that runs its own xtc runtime; the
 *	two are joined by the xtc_osproc control socketpair.  The parent
 *	gets an xtc_xpid_t handle for the child.  Over that handle it can:
 *	  - xtc_xsend  a message that the child's runtime delivers to its
 *	    root proc (a byte payload, copied);
 *	  - xtc_xmonitor / xtc_xlink to the child so a child CRASH or EXIT
 *	    surfaces as a normal xtc DOWN in the monitoring fiber, with the
 *	    signal / exit code decoded from the child's waitpid status;
 *	  - observe XTC_DOWN_KIND_NOCONNECTION if the control channel dies
 *	    before a clean exit is seen (the local analog of node-down).
 *
 *	The child side runs xtc_xproc_child_main(ctrl_fd, root_fn, arg): it
 *	stands up a relay that receives xtc_xsend payloads and delivers
 *	them to root_fn's proc, and it exits when the parent closes the
 *	channel or the child's own runtime finishes.
 *
 *	PLATFORM: POSIX only (fork + socketpair + waitpid); the Windows
 *	xtc_osproc entry points decline, and so do these.
 */

#ifndef XTC_XPROC_H
#define XTC_XPROC_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

typedef struct xtc_xproc xtc_xproc_t;

/* A cross-process pid: the child handle plus (reserved) a node-local pid
 * within that child.  For the MVP a message addresses the child's root
 * proc; local_pid is reserved for the future per-child pid routing the
 * distributed design specifies. */
typedef struct xtc_xpid {
	xtc_xproc_t *child;      /* the fork'd child this pid lives in */
	xtc_pid_t    local_pid;  /* reserved: pid within the child (0 = root) */
} xtc_xpid_t;

/* The child's entry: stand up the relay on `ctrl_fd`, spawn root_fn as
 * the child's root proc, and pump the relay + loop until the parent
 * closes the channel.  Call this from the fn passed to xtc_xspawn (it is
 * the child-side counterpart).  Returns the child's exit code. */
typedef void (*xtc_xproc_root_fn)(void *arg);

/*
 * PUBLIC: int  xtc_xspawn __P((xtc_loop_t *, const char *, xtc_xproc_root_fn, const void *, size_t, xtc_xproc_t **));
 * PUBLIC: int  xtc_xproc_register_entry __P((const char *, xtc_xproc_root_fn));
 * PUBLIC: int  xtc_xspawn_entry __P((xtc_loop_t *, const char *, const char *, const void *, size_t, xtc_xproc_t **));
 * PUBLIC: void xtc_xproc_destroy __P((xtc_xproc_t *));
 * PUBLIC: long xtc_xproc_os_pid __P((const xtc_xproc_t *));
 * PUBLIC: int  xtc_xsend __P((xtc_xproc_t *, const void *, size_t));
 * PUBLIC: int  xtc_xmonitor __P((xtc_xproc_t *, uint64_t *));
 * PUBLIC: int  xtc_xlink __P((xtc_xproc_t *));
 * PUBLIC: int  xtc_xproc_child_main __P((int, xtc_xproc_root_fn, void *));
 * PUBLIC: int  xtc_xproc_win_child_maybe __P((int, char **));
 */

/*
 * Fork a child that runs `root_fn`.  The child's runtime is stood up by
 * the library; root_fn receives, as its arg, a pointer to a copy of the
 * `arg`/`arg_len` bytes the parent passed (or NULL if arg_len == 0).
 * The parent gets an xtc_xproc handle in *out.  Must be called from a
 * fiber on `loop` (it registers a monitor-relay proc there).
 *
 * POSIX ONLY (fork preserves the address space, so a raw function
 * pointer is meaningful in the child).  Returns XTC_E_NOSYS on Windows,
 * where a child is a fresh process image and a function pointer does not
 * survive -- use xtc_xspawn_entry there (and portably).
 */
int  xtc_xspawn(xtc_loop_t *loop, const char *name,
                xtc_xproc_root_fn root_fn,
                const void *arg, size_t arg_len,
                xtc_xproc_t **out);

/*
 * Register a child root function under a name in a process-global table.
 * The SAME binary (parent and any re-exec'd child) resolves the name to
 * the same function, so this is the portable bridge that works where a
 * raw function pointer cannot survive process creation (Windows).  Call
 * it once, early, in code that runs in BOTH the parent and the child
 * image (e.g. before parsing argv in main, or a constructor).  Names are
 * short strings; re-registering a name replaces the binding.  Returns
 * XTC_OK, XTC_E_INVAL, or XTC_E_RESOURCE if the table is full.
 */
int  xtc_xproc_register_entry(const char *name, xtc_xproc_root_fn fn);

/*
 * Spawn a child that runs the root function registered under `entry`
 * (see xtc_xproc_register_entry).  Portable: on POSIX it forks and looks
 * the name up in the child; on Windows it CreateProcess-es a re-exec of
 * this binary, which looks the name up in its own copy of the registry.
 * `arg`/`arg_len` are copied and delivered to the root function as its
 * arg.  Otherwise identical to xtc_xspawn (monitor, send, destroy all
 * work the same).  Returns XTC_E_NOTFOUND if `entry` is not registered.
 */
int  xtc_xspawn_entry(xtc_loop_t *loop, const char *name, const char *entry,
                      const void *arg, size_t arg_len, xtc_xproc_t **out);

/* Tear down the handle: signal + reap the child if still running, close
 * the channel, free the handle.  Idempotent. */
void xtc_xproc_destroy(xtc_xproc_t *p);

/* The child's OS pid (for logging), or -1. */
long xtc_xproc_os_pid(const xtc_xproc_t *p);

/* Send a byte payload (copied) to the child's root proc.  It arrives in
 * the child root's mailbox as an ordinary xtc_recv message.  Returns
 * XTC_E_INVAL if the channel is closed. */
int  xtc_xsend(xtc_xproc_t *p, const void *msg, size_t len);

/*
 * Monitor the child from the calling fiber: when the child exits or
 * crashes -- or its control channel dies -- the caller receives a normal
 * xtc DOWN message (decode with xtc_down_decode_ex).  The DOWN's kind is
 * XTC_DOWN_KIND_EXIT / _SIGNAL decoded from the child's waitpid status,
 * or XTC_DOWN_KIND_NOCONNECTION if the channel died first.  *out_ref (if
 * non-NULL) receives the monitor reference.  Must be called from a fiber.
 */
int  xtc_xmonitor(xtc_xproc_t *p, uint64_t *out_ref);

/*
 * Bidirectionally LINK the calling fiber to the child (like xtc_link,
 * across the fork boundary): if the child exits or crashes the linking
 * fiber receives an EXIT signal (decode with xtc_down_decode_ex), and if
 * the linking fiber / parent process dies the child observes its control
 * channel close and shuts down.  Must be called from a fiber.  Use
 * xtc_xmonitor instead when you want a one-way DOWN without binding the
 * caller's own fate to the child.
 */
int  xtc_xlink(xtc_xproc_t *p);

/*
 * Child-side entry.  Call this from the child body (the process the fork
 * lands in) with the inherited control fd, the root proc function, and
 * its arg.  It stands up a child runtime, delivers parent xtc_xsend
 * payloads into root_fn's mailbox, and returns the child's exit code
 * when the channel closes or root_fn's proc finishes.  Most embedders do
 * not call this directly -- xtc_xspawn arranges the child side -- but it
 * is public so a re-exec'd child (a future extension) can re-enter.
 */
int  xtc_xproc_child_main(int ctrl_fd, xtc_xproc_root_fn root_fn, void *arg);

/*
 * Windows only.  The re-exec'd child image calls this early in main():
 * if the cross-process-child sentinel argv is present it connects the
 * control channel, receives its arg, runs the registered entry, and
 * _exit()s -- never returning.  Otherwise it is a no-op returning 0 and
 * normal startup continues.  On POSIX it is a no-op that returns 0 (the
 * child is fork'd, not re-exec'd, so there is no sentinel to detect).
 * Wire it as the first statement of main() in a binary that will host
 * xtc_xspawn_entry children.
 */
int  xtc_xproc_win_child_maybe(int argc, char **argv);

#endif /* XTC_XPROC_H */
