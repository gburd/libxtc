# Man page coverage TODO

This document tracks man pages that need to be written.

## Existing man pages

### Section 3 (Library functions)

- `__os_alloc.3` -- low-level allocator
- `__os_atomic.3` -- atomic operations
- `__os_mutex.3` -- mutex primitives
- `__os_thread.3` -- thread primitives
- `__os_time.3` -- time primitives
- `__os_tls.3` -- thread-local storage
- `xtc_async.3` -- async coroutines
- `xtc_exec.3` -- work-stealing executor
- `xtc_io.3` -- I/O multiplexer
- `xtc_loop.3` -- event loop
- `xtc_strerror.3` -- error strings
- `xtc_version_components.3` -- version components
- `xtc_version_string.3` -- version string

### Section 7 (Miscellaneous)

- `xtc.7` -- library overview
- `xtc-abi-stability.7` -- ABI stability guarantees
- `xtc-build.7` -- build instructions

## Missing man pages (22 total)

### Concurrency primitives

- [ ] `xtc_proc.3` -- BEAM-style lightweight processes
- [ ] `xtc_chan.3` -- typed channels (MPSC/MPMC)
- [ ] `xtc_sync.3` -- synchronization primitives (barriers, latches)
- [ ] `xtc_mctx.3` -- manual context switching

### Locking

- [ ] `xtc_lwlock.3` -- lightweight locks (reader/writer)
- [ ] `xtc_lrlock.3` -- leader/reader locks
- [ ] `xtc_lockmgr.3` -- lock manager (deadlock detection)

### Memory

- [ ] `xtc_slab.3` -- slab allocator
- [ ] `xtc_rcu.3` -- read-copy-update

### Configuration and observability

- [ ] `xtc_log.3` -- structured logger
- [ ] `xtc_cfg.3` -- runtime configuration (GUC-style)
- [ ] `xtc_inject.3` -- fault injection
- [ ] `xtc_pdict.3` -- process dictionary

### Resources

- [ ] `xtc_res.3` -- resource management (ref-counted handles)

### OTP patterns

- [ ] `xtc_app.3` -- application framework
- [ ] `xtc_reg.3` -- process registry
- [ ] `xtc_svr.3` -- gen_server pattern
- [ ] `xtc_sup.3` -- supervisor pattern
- [ ] `xtc_orc.3` -- orchestration layer overview

### Networking

- [ ] `xtc_net.3` -- TCP/UDP/UDS networking
- [ ] `xtc_tls.3` -- TLS support (OpenSSL backend)

## Priority

High priority (needed for M16 PostgreSQL adapter):
1. `xtc_proc.3`
2. `xtc_lockmgr.3`
3. `xtc_slab.3`
4. `xtc_log.3`
5. `xtc_cfg.3`

Medium priority (public API, commonly used):
6. `xtc_chan.3`
7. `xtc_net.3`
8. `xtc_tls.3`
9. `xtc_sup.3`
10. `xtc_svr.3`

Lower priority (specialized use):
11-22. Everything else
