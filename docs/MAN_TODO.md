# Man page coverage status

## Section 3 (library functions) -- done

xtc primitive man pages:

* `xtc_proc.3` -- BEAM-style processes, mailbox, links, monitors
* `xtc_chan.3` -- typed channels (oneshot/MPSC/MPMC/watch/broadcast)
* `xtc_lwlock.3` -- lightweight reader/writer lock
* `xtc_lrlock.3` -- left-right wait-free-read lock
* `xtc_lockmgr.3` -- transactional lock manager + deadlock detector
* `xtc_rcu.3` -- epoch-based read-copy-update
* `xtc_sync.3` -- async sync primitives (amutex/rwlock/notify/sem/barrier/gate)
* `xtc_slab.3` -- slab + magazine allocator
* `xtc_mctx.3` -- hierarchical memory contexts
* `xtc_res.3` -- resource accountant with caps
* `xtc_stats.3` -- counters/gauges/histograms
* `xtc_log.3` -- async ring-buffer logger
* `xtc_cfg.3` -- typed runtime configuration
* `xtc_inject.3` -- fault-injection points
* `xtc_pdict.3` -- per-process dictionary
* `xtc_reg.3` -- name registry
* `xtc_supervisor.3` -- OTP supervisor
* `xtc_app.3` -- application container
* `xtc_svr.3` -- gen_server analog
* `xtc_net.3` -- TCP/UDS networking helpers
* `xtc_tls.3` -- TLS over xtc_io

L0/L1/L2 lower-layer pages already shipped before:
`__os_alloc.3`, `__os_atomic.3`, `__os_mutex.3`, `__os_thread.3`,
`__os_time.3`, `__os_tls.3`, `xtc_io.3`, `xtc_loop.3`, `xtc_async.3`,
`xtc_exec.3`, `xtc_strerror.3`, `xtc_version_components.3`,
`xtc_version_string.3`.

## Section 7 (overview / build) -- done

* `xtc.7` -- library overview
* `xtc-abi-stability.7` -- ABI/SemVer policy
* `xtc-build.7` -- build instructions

## Pages NOT shipped (intentional)

* No man page for `xtc_int.h` -- that's the internal header, not a
  public API.
* No per-backend pages for io_uring/epoll/kqueue/IOCP -- backends
  are configure-time-selected; the user-facing API is `xtc_io`.
* No page for `xtc_tailcall.h` -- macro-only header, documented
  inline + via `scripts/check-tailcalls.sh`.

## Future additions

When new public primitives ship, each gets a corresponding man3
page following the same template:

* SH NAME with all entry points
* SH SYNOPSIS with `.In` headers and `.Fn` signatures
* SH DESCRIPTION explaining the model
* CHOOSING vs section comparing to alternatives
* SH EXAMPLES with a copy-paste recipe
* SH SEE ALSO cross-refs
* SH HISTORY noting first-appearance version

The `dist/test_man_coverage.sh` test verifies every public-symbol
file has a corresponding man page.

## Coverage status (2026-07)

Whole-subsystem pages that were MISSING and are now added:
xtc_blocking(3), xtc_pkey(3), xtc_iosched(3), xtc_dio_sched(3).
Every public header now has at least one man3 page.

Known remaining gap (tracked, not yet closed): the coverage gate
(test_man_coverage.sh) currently scans only the umbrella xtc.h, so it
does not catch SECONDARY functions of a subsystem that has a page but
does not list them as `.Nm`/`.Fn` entries.  A stricter gate that scans
the full installed header set and checks each PUBLIC function is
mentioned in some page found ~120 such functions -- e.g. the
xtc_amutex_* / xtc_arwlock_* / xtc_sem_* / xtc_gate_* family (belongs
in xtc_sync(3)), the xtc_cfg_get_*/set_* accessors (xtc_cfg(3)), the
xtc_chan_* variants (xtc_chan(3)), xtc_mctx_* helpers (xtc_mctx(3)),
xtc_lock_upgrade/downgrade/release_all (xtc_lockmgr(3)), xtc_net_udp_*
(xtc_net(3)), etc.  These are documented at the subsystem level but
not enumerated per-entry-point.  Closing this is a mechanical pass
(add `.Nm` aliases + one `.Fn` per function to the existing pages),
after which the stricter gate can be enabled to fail the build.  The
gate rewrite is drafted; it is held back so it does not fail `make
check` until the pages are filled in.
