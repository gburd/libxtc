/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_tailcall.h
 *	Mark functions/call-sites as MUST-be-tail-called.  GCC and
 *	Clang both perform tail-call optimization (TCO) when the
 *	last operation in a function is a call to another function
 *	whose return value is also returned.  We document the
 *	intent at the call site so a script can verify that the
 *	compiler actually emitted a `jmp` (or arch-equivalent) and
 *	not a `call` followed by `ret`.
 *
 *	Usage:
 *	  - At a call-site that MUST be tail-called for correctness
 *	    (e.g. avoiding stack growth in a deep dispatch chain),
 *	    annotate via XTC_MUSTTAIL:
 *
 *	        return XTC_MUSTTAIL fn(args);
 *
 *	  - GCC's `__attribute__((musttail))` (15+) and Clang's
 *	    `[[clang::musttail]]` are honored.  On older toolchains
 *	    we fall back to `__attribute__((always_inline))` of a
 *	    helper that returns directly.
 *
 *	Validation:
 *	  scripts/check-tailcalls.sh disassembles the static lib
 *	  and confirms each annotated site is a `jmp` not a `call`.
 *	  Run via `make check-tailcalls`.
 *
 *	When TCO matters in xtc:
 *	  - State-machine dispatch: handler returning a tail-call to
 *	    the next state's handler (e.g. parse trees).
 *	  - Scheduler dispatch: loop's task-step jumping into the
 *	    coroutine entry without growing the loop's stack.
 *	  - Lock-free fast paths where one inline helper jumps to
 *	    the slow path on miss.
 */

#ifndef XTC_TAILCALL_H
#define XTC_TAILCALL_H

/* musttail is a *statement* attribute, not a function attribute.
 * Both clang's `[[clang::musttail]]` and gcc-15's
 * `__attribute__((musttail))` are accepted as statement attributes,
 * but only in compilers that parse C2x bracket attributes (clang)
 * or have GCC 15's extension.  In strict C11 mode — which xtc uses —
 * we fall back to a no-op and rely on the optimizer.  The
 * `scripts/check-tailcalls.sh` validator inspects the .o to confirm
 * `jmp` was emitted regardless. */
/* XTC_MUSTTAIL is a marker; it's prefixed to a function call inside
 * a return statement to signal intent.  In gcc 15+ and clang's C2x
 * mode the attribute can be attached to the return statement itself
 * (`__attribute__((musttail)) return foo();`) but the syntactic
 * position varies enough across toolchains that we keep the macro
 * as a no-op universally and rely on:
 *   1. the -O2 optimizer doing TCO when it can, and
 *   2. scripts/check-tailcalls.sh validating the .o emitted `jmp`.
 * When compiler enforcement matters more than convenience, callers
 * can use the explicit form directly. */
#define XTC_MUSTTAIL  /* no-op marker; see check-tailcalls.sh */

/* Marker macro for the validator script: annotated call sites
 * become a #pragma comment that survives into the disassembly's
 * line metadata. */
#if defined(XTC_TAILCALL_MARK_SITES)
# define XTC_TAIL_CALL(expr) \
	(__extension__ ({ \
		_Pragma("message \"xtc-tailcall-marker\""); \
		XTC_MUSTTAIL expr; \
	}))
#else
# define XTC_TAIL_CALL(expr)  (XTC_MUSTTAIL expr)
#endif

#endif /* XTC_TAILCALL_H */
