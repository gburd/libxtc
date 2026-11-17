---
title: Deprecations
parent: Reference
nav_order: 14
lede: >-
  Every name deprecated in 2.x, what replaces it, and when it goes.
permalink: /reference/deprecations/
---
libxtc 2.2 renames part of its API so that one verb has one meaning
(the conventions in [ABI stability]({{ '/reference/abi-stability/' |
relative_url }})).  Each old name below still works through the rest of
2.x and is **removed in 3.0**.  Moving a program to the new names is a
search and replace, except where the table says the arguments changed.

How an old name keeps working:

- **A function** stays an exported symbol in its original version node
  (`XTC_2.0`), so a binary built against 2.1 links and runs against 2.2
  with no rebuild.  It is a thin wrapper that calls the new function,
  and its declaration carries `XTC_DEPRECATED("use xtc_new_name")`, so a
  rebuild warns (`-Wdeprecated-declarations` on gcc and clang, C4996 on
  MSVC).  The new name is a new symbol in `XTC_2.2`.
- **A type name** is a `typedef` of the new one, marked deprecated where
  the compiler can mark a typedef (gcc, clang, MSVC).
- **An enumerator** is an alias of the new one with the same value,
  marked `XTC_DEPRECATED_ENUM` (gcc and clang; MSVC accepts it silently).
- **A macro** is a `#define` of the new one, marked
  `XTC_DEPRECATED_MACRO` (clang only: no other compiler can mark a
  macro).
- **Frozen** names (the lock layer: `docs/abi-stability.md`, "Frozen
  surfaces") get the new spelling as an alias and keep the old one with
  no warning.  3.0 makes the alias the name.

`XTC_NO_DEPRECATION_WARNINGS`, defined before the first libxtc include,
silences every one of these marks while a large codebase migrates.
`test/abi/test_deprecated_bridge.c` calls each old name once to prove it
still works.

**Destructors.**  A new `*_destroy` returns `void`, and `NULL` is a
no-op: a destructor cannot fail, so there is nothing to check.  The old
`int`-returning `*_fini` keeps its contract exactly, including
`XTC_E_INVAL` for `NULL` and `XTC_OK` otherwise, so code that tests its
result keeps working until it moves to the new name.

## Renamed in 2.2 (removed in 3.0)

<!-- renames:begin -->
| Old (deprecated in 2.2) | New (2.2) | Kind | Notes |
|---|---|---|---|
| `xtc_alloc_audit_enable()` | `xtc_runtime_set_alloc_audit()` | function |  |
| `XTC_APP_OPTS_DEFAULT` | `XTC_APP_OPTS_INIT` | macro |  |
| `xtc_batch_close()` | `xtc_batch_shutdown()` | function |  |
| `xtc_blocking_pool_size()` | `xtc_runtime_set_blocking_threads()` | function | "too late" is XTC_E_STATE under the new name, XTC_E_INVAL under the old |
| `xtc_bloom_fini()` | `xtc_bloom_destroy()` | function |  |
| `xtc_bloom_init()` | `xtc_bloom_create()` | function | arguments reordered: (n_expected, fp_rate, out) |
| `xtc_chan_demand_close()` | `xtc_chan_demand_shutdown()` | function |  |
| `xtc_chan_mpmc_close()` | `xtc_chan_mpmc_shutdown()` | function |  |
| `xtc_chan_mpsc_close()` | `xtc_chan_mpsc_shutdown()` | function |  |
| `xtc_dispatch_handle_free()` | `xtc_dispatch_detach()` | function |  |
| `xtc_exec_fini()` | `xtc_exec_destroy()` | function | the new name returns void; the old one returns XTC_E_INVAL for NULL, else XTC_OK |
| `xtc_exec_init()` | `xtc_exec_create()` | function | arguments changed: (const xtc_exec_opts_t *opts, out), n_loops is opts->n_loops |
| `xtc_gate_close()` | `xtc_gate_shutdown()` | function |  |
| `xtc_hll_fini()` | `xtc_hll_destroy()` | function |  |
| `xtc_hll_init()` | `xtc_hll_create()` | function | arguments reordered: (precision, out) |
| `xtc_io_fini()` | `xtc_io_destroy()` | function | the new name returns void; the old one returns XTC_E_INVAL for NULL, else XTC_OK |
| `xtc_io_init()` | `xtc_io_create()` | function |  |
| `xtc_io_set_iowq_max_workers()` | `xtc_runtime_set_iowq_workers()` | function |  |
| `XTC_LOCKMGR_OPTS_DEFAULT` | `XTC_LOCKMGR_OPTS_INIT` | frozen | frozen lock layer: an alias, no warning; 3.0 keeps only XTC_LOCKMGR_OPTS_INIT |
| `XTC_LOG_OPTS_DEFAULT` | `XTC_LOG_OPTS_INIT` | macro |  |
| `xtc_loop_fini()` | `xtc_loop_destroy()` | function | the new name returns void; the old one returns XTC_E_INVAL for NULL, else XTC_OK |
| `xtc_loop_init()` | `xtc_loop_create()` | function |  |
| `xtc_pg_join()` | `xtc_pg_subscribe()` | function |  |
| `xtc_pg_leave()` | `xtc_pg_unsubscribe()` | function | a non-member is XTC_E_NOTFOUND (the old name: XTC_E_INVAL) |
| `xtc_preempt_set_involuntary()` | `xtc_runtime_set_preempt_involuntary()` | function |  |
| `xtc_reg_register()` | `xtc_reg_put()` | function | call it as xtc_reg_put(r, name, pid, NULL, 0); a taken name is XTC_E_EXIST (the old name: XTC_E_INVAL) |
| `xtc_reg_register_dup()` | `xtc_reg_put()` | function | call it as xtc_reg_put(r, key, pid, NULL, XTC_REG_DUP) |
| `xtc_reg_register_mon()` | `xtc_reg_put()` | function | call it as xtc_reg_put(r, name, pid, NULL, XTC_REG_MONITOR) |
| `xtc_reg_unregister()` | `xtc_reg_del()` | function | call it as xtc_reg_del(r, name, XTC_PID_NONE); a miss is XTC_E_NOTFOUND (the old name: XTC_E_INVAL) |
| `xtc_reg_unregister_pid()` | `xtc_reg_del()` | function | same arguments; a miss is XTC_E_NOTFOUND (the old name: XTC_E_INVAL) |
| `xtc_reg_whereis()` | `xtc_reg_get()` | function | same arguments; a miss is XTC_E_NOTFOUND (the old name: XTC_E_INVAL) |
| `XTC_RES_CAPS_DEFAULT` | `XTC_RES_CAPS_INIT` | macro |  |
| `xtc_scope_close()` | `xtc_scope_leave()` | function |  |
| `xtc_scope_open()` | `xtc_scope_enter()` | function |  |
| `xtc_set_stack_size()` | `xtc_runtime_set_stack_size()` | function |  |
| `XTC_SLAB_OPTS_DEFAULT` | `XTC_SLAB_OPTS_INIT` | macro |  |
| `xtc_slab_pressure_listen()` | `xtc_slab_pressure_create()` | function | the handle is now returned: pass an out pointer and keep (or never destroy) it |
| `xtc_slab_pressure_listen_ex()` | `xtc_slab_pressure_create()` | function | same arguments |
| `xtc_slab_pressure_stop()` | `xtc_slab_pressure_destroy()` | function | the new name returns void; the old one returns XTC_E_INVAL for NULL, XTC_E_NOSYS without PSI, else XTC_OK |
| `xtc_stack_reclaim_disable()` | `xtc_runtime_set_stack_reclaim()` | function | call it with 0 |
| `xtc_stack_reclaim_enable()` | `xtc_runtime_set_stack_reclaim()` | function | keep_bytes 0 now means off; the old 0 (one page) is the page size |
| `xtc_stack_reclaim_enabled()` | `xtc_runtime_get_stack_reclaim()` | function | nonzero (the keep margin) when on |
| `xtc_stack_size()` | `xtc_runtime_get_stack_size()` | function |  |
| `XTC_SUP_OPTS_DEFAULT` | `XTC_SUP_OPTS_INIT` | macro |  |
| `XTC_TCP_OPTS_DEFAULT` | `XTC_TCP_OPTS_INIT` | macro |  |
| `xtc_timer_release()` | `xtc_timer_detach()` | function |  |
| `xtc_tnt_start()` | `xtc_tnt_run()` | function | same arguments; it blocks until the system stops, as before |
| `xtc_xlink()` | `xtc_xproc_link()` | function |  |
| `xtc_xmonitor()` | `xtc_xproc_monitor()` | function |  |
| `xtc_xsend()` | `xtc_xproc_send()` | function |  |
| `xtc_xspawn()` | `xtc_xproc_spawn()` | function |  |
| `xtc_xspawn_entry()` | `xtc_xproc_spawn_entry()` | function |  |
<!-- renames:end -->

## Planned for 3.0 (not done in 2.x)

- `xtc_arwlock_*` / `xtc_arwlock_t` (the fiber-parking reader/writer
  lock) becomes `xtc_rwlock_*` / `xtc_rwlock_t`, reusing the name the
  blocking OS-thread lock gave up in 2.2 (it is `xtc_thrd_rwlock_*`
  now).  3.0 removes the deprecated `xtc_rwlock_*` bridges first, so a
  2.x program that still calls them fails to build against 3.0 instead
  of silently changing from a thread lock to a fiber lock.
- The frozen lock-layer aliases (`XTC_LWLOCK_*`, `xtc_lwlock_try_acquire`)
  become the only names.
