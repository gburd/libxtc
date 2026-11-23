# libxtc 2026.11.23

libxtc 2026.11.23 is a development release from `main`, published as a
pre-release.  Its ABI level is 2.3.0: the SONAME stays libxtc.so.2 (the
file is libxtc.so.2.3.0), the library exports 2.2.0's functions at
their old version nodes plus 3 new ones in XTC_2.3, and none was
removed (abidiff against 2.2.0: status 4, 0 removed, 0 changed, 3
added).  `xtc_version_string()` returns "2026.11.23";
`xtc_version_components()` and the XTC_VERSION_MAJOR, _MINOR, and
_PATCH macros give 2.3.0.

The LTS line is 2.1.x on `lts/2.1`, and its newest release stays
"Latest".  A development release ships source artefacts only (the
source tarball, the single-file amalgamation, and the man pages), no
.deb or .rpm, so it never replaces an LTS install.  It is supported
until the next development release.  See docs/abi-stability.md,
"Long-term support and release lines", and SECURITY.md.

This is the first release named by date: v2.2.0 was the last SemVer
release from `main`.  A pkg-config check `xtc >= 2.3` accepts it; `xtc
< 3` does not (pkg-config compares 2026.11.23 as a version), so test
XTC_VERSION_MAJOR and _MINOR for the ABI level.

The library fixes below that also apply to 2.1 shipped on the LTS line
as well: the forged DOWN, the NOMEM put-back, and the NOCONNECTION
decode in 2.1.4; the dropped-id fix in 2.1.5 (without the reclaim of a
dropped table's rows); the victim IDs, the RNG seed, and the xproc
shadow in 2.1.6.

## Behaviour changes

- Library receivers of DOWN, link EXIT, and start acknowledgements act
  only on messages the library sent.  Code that hand-crafted such a
  message with xtc_send to drive a supervisor, the registry reaper, a
  call wait, or xtc_launch no longer gets a reaction.
- A DOWN or link EXIT of kind XTC_DOWN_KIND_NOCONNECTION decodes as
  NOCONNECTION, where it used to decode as SIGNAL with signal 4, and
  the xtc_xproc shadow reports NOCONNECTION where it reported EXIT 4.
- A custom deadlock-victim picker gets locker IDs, where it used to get
  the detector's node indices.
- sqlxtc example: an autocommit statement that changes several rows
  commits all of them or none.  INSERT VALUES is now held to the same
  100,000-row statement limit as UPDATE, DELETE, and INSERT ... SELECT.
  A multi-row autocommit INSERT that loses a key race reports UNIQUE.
  The index format moves from 2 to 3: a store closed cleanly by 2.2.0
  rebuilds its indexes once at the first open.
- rexis example: `--persist` logs every command that changes the
  keyspace, in a new log file, DIR/bitcask.v2.  A directory from an
  older rexis (DIR/bitcask.data) is converted at the first start, and
  an older binary cannot read a converted directory (it starts empty).
  A log that cannot be reloaded in full stops the server at startup, a
  write the log cannot take stops it with exit status 74 before the
  reply, and damage other than a torn tail refuses the start.

## New API (XTC_2.3)

- xtc_recv_mark and xtc_recv_match_since: a receive mark records the
  position of the last message that has arrived, and a receive given
  the mark starts after it, so a reply wait does not test the backlog.
  A request-reply with 100,000 unrelated messages queued took 1.42 ms
  with xtc_recv_match and 626 ns with the mark on a 48-core x86-64
  host.  A stale mark, or 0, walks the whole queue.  New DST test
  test_sim_recv_mark, planted bug 76.
- xtc_recv_match_ex, xtc_match_ex_fn, and xtc_msg_info_t: a selective
  receive whose predicate sees the sender's pid and XTC_MSG_SYS, which
  the mailbox sets only on a DOWN, link EXIT, ROOM, or start
  acknowledgement the library sent.  A message is a DOWN when
  xtc_down_decode_ex accepts it and XTC_MSG_SYS is set.

## Library fixes

- A user message shaped like a DOWN was taken for a death.  The
  library's receivers of DOWN, link EXIT, and start acknowledgements
  (the supervisor, the registry reaper, the xtc_svr_call and
  xtc_fsm_call waits, and xtc_launch) tested the bytes alone, so an
  ordinary xtc_send made a supervisor start a second copy of a running
  child, the reaper drop a running proc's XTC_REG_MONITOR names, or a
  call end with XTC_E_NOTFOUND.  They now act only on messages the
  library marked.  No API, symbol, or layout change.  New DST test
  test_sim_forged_down; planted bugs 77 and 78.  A plain receive walks
  the save queue in a loop of its own and tests the system flag only
  when a caller asks for it.  Cost: a same-loop send and receive takes
  about 19 to 22 ns more on a bare-metal x86-64 host (perf-gate
  micro_proc_send_recv against 2.2.0, two runs: 709.6 to 729.0 ns and
  708.1 to 730.4 ns, 2.7% and 3.2%, under the cell's 10% threshold; no
  other cell regressed).
- A receive whose copy of an inline payload fails with XTC_E_NOMEM put
  the message back at the head of the save queue, so the next xtc_recv
  returned it ahead of older messages.  It now goes back in arrival
  order.  New test /m8/proc/recv_nomem_keeps_order.
- NOCONNECTION: a DOWN or link EXIT of kind XTC_DOWN_KIND_NOCONNECTION
  decoded as SIGNAL with signal 4; it now decodes as NOCONNECTION.  The
  xtc_xproc shadow proc reports NOCONNECTION, not EXIT 4, when waiting
  for the child fails, and the Windows shadow reports NOCONNECTION when
  the child's exit code cannot be read or its wait cannot be registered
  (there xtc_xproc_monitor and xtc_xproc_link returned XTC_E_IO and
  left the shadow parked).  New tests /m8/proc/down_decode_noconnection
  and /m10.10/xproc/wait_fail_noconnection, and a case in the MSVC
  smoke test.
- xtc_lockmgr: a custom victim picker (XTC_LOCK_VICTIM_CUSTOM) was
  handed the detector's internal node indices (0, 1, ...) instead of
  the locker IDs its contract names.  The victim chosen was right, but
  a picker that chose, looked up, or logged by ID got the wrong ones.
  It now gets the IDs.  No API, symbol, or layout change.  New test
  /m13/lockmgr/victim_cycle.
- The RNG: an un-seeded thread's first draw made outside a simulator
  run but with the virtual clock on (a DST harness's setup) mixed the
  address of the thread's state into its seed, so it carried the
  process's ASLR layout into the run, and the determinism guard could
  not see it.  Under the virtual clock the seed is now the virtual time
  and a constant.  Off the virtual clock nothing changes.  It made one
  sqlxtc DST seed replay differently in about 1 process in 40; that
  seed is pinned, and the sqlxtc DST tiers now also replay a seed in a
  freshly started process.

## sqlxtc example

- Index keys of any length.  A key longer than one B-tree key (497
  bytes on 4 KiB pages) got no index entry, so a UNIQUE column accepted
  two identical long values and an index lookup missed the row; base
  rows were never lost.  A long key is now stored as its first 476
  bytes plus a keyed SipHash-1-3 of the whole key, and every match is
  checked against the row.  The index format moves from 2 to 3.  New
  test test_uniq_long, DST test test_sim_sqlxtc_uniq, planted bugs 72
  and 73.  The nested-loop join no longer reads one element before its
  order array when it ends.
- Read errors are errors.  A failed B-tree read in a scan, an index
  scan, a UNIQUE probe, a point read, a catalog read, an INSERT key
  check, a write-write check, a max-rowid read, or a correlated
  subquery used to read as "absent", "the end", "empty", or 0, and the
  statement went on.  Each now fails the statement.  A structural gate,
  test/test_rderr_sites.py (make test-rderr-sites, part of make test),
  finds every call of a B-tree read that can fail, and of every
  function that wraps one, and fails unless its error is returned or
  tested, or the line carries a reviewed RDERR_OK marker.  Planted bugs
  80 to 86.  DDL statements now run one at a time, under one lock.
- Statement atomicity.  An autocommit statement that changes several
  rows runs as an implicit transaction, so a failure partway (a read
  error, a UNIQUE violation, a row too large) or a kill of the
  connection leaves none of its rows; before, the rows before the
  failure stayed committed.  A one-row statement commits nothing when
  one of its reads failed or a kill is pending.  A multi-row INSERT
  that loses a key race re-runs and reports UNIQUE, not a serialization
  failure.  The one-fsync commit holds while the statement's rows stay
  under 256 KiB.  An UPDATE, DELETE, INSERT ... SELECT, or INSERT
  VALUES over 100,000 rows fails with "too many rows"; INSERT VALUES
  had no limit before.  A statement discarded by a kill fails with
  "statement cancelled: the connection was killed", and a DDL statement
  waiting on the schema lock at shutdown fails with "cannot change the
  schema: the storage engine is shutting down".  New tests
  test_stmt_atomic and test_insert_race; test_crash_oracle kills
  multi-row statements, at random and at planned points on each side
  of the commit record; planted bug 87.
- A dropped table's or index's id is never reused.  A new table or
  index took one past the largest live catalog id, and once the DROP's
  tombstone was gone (a clean close's GC, or a crash rebuild) the next
  CREATE took the dropped id back, and a new table showed the dropped
  table's rows.  Both catalogs now keep a high-water row at rowid 0,
  which DROP raises, and GC and the log dump reclaim a dropped table's
  rows.  Planted bug 79.
- The online checkpoint reported too few frames: it counted the frames
  it copied before its quiesced finish, so it missed every commit
  acknowledged after the last catch-up round.  The frames were always
  copied, and recovery restored every acknowledged row; only the count
  was wrong.  The count now includes the finish, and test_ckpt_online,
  which failed about 1 run in 200 under heavy load, holds each
  checkpoint until a commit is acknowledged inside it.
- Storage framework.  The building blocks of a log-structured row
  store landed with their own tests: a segmented WAL with 32-bit
  segment and 32-bit offset WAL addresses and segment retention
  (test_wal_seg), immutable run files (test_run), Elias-Fano chunks
  (test_ef), the keydir of resident map, delta, and run set
  (test_keydir), and the MANIFEST with the nursery flush (test_manifest).
  None of it is wired into the engine yet: sqlxtc still stores rows in
  its B-tree and writes its one-file WAL as before.
- The Makefile generates header dependencies (-MMD -MP).  The
  hand-kept lists had rotted, so a tree rebuilt without a clean could
  link a stale object.
- make test-valgrind runs every C test of make test under valgrind
  memcheck against an epoll libxtc (it refuses an io_uring build, which
  valgrind does not model), and the tests whose timing bounds or
  address-space caps did not hold under valgrind now allow for it.

## rexis example

- `--persist` logs every command that changes the keyspace.  It used
  to log only a SET without a TTL and DEL, so INCR results, TTLs,
  lists, hashes, and FLUSHDB were lost on a restart.  Each command now
  logs the key's resulting state (its value, type, and expiry, or a
  tombstone), FLUSHDB logs one flush record, and one command is one log
  record, so a crash in the middle of a command leaves the keyspace
  before it or after it.  An expiry is logged on the wall clock, so a
  key that expired while the server was down does not come back.
- The log format is v2, in DIR/bitcask.v2.  Every record header carries
  a CRC over its timestamp and lengths, checked before the lengths are
  trusted, so a damaged length is never taken for a torn tail.  A
  DIR/bitcask.data from an older rexis is converted at the first start,
  through a temp file renamed into place, and a crash at any step of
  the conversion resumes at the next start.
- The damage rule: a crash or kill -9 leaves a torn tail, which is cut
  off with a line on stderr; any other damage refuses the start, names
  the offsets, and leaves the file as it was.  A power loss can leave a
  hole in the middle of the log, so it can refuse the start and need an
  operator.  The README's Persistence section gives the exact rule.
- A write the log refuses stops the server with exit status 74 before
  the reply, so no unlogged write is acknowledged.  The durability
  promise is written down: an acknowledged command survives a crash or
  kill -9 of the server, not a power loss; a clean shutdown fsyncs.
- test_rexis_budgets waits until the server has processed its closed
  connections before it stops the server.  The old stop raced
  io_uring poll completions still unread, which the io_uring release
  gate reported as POLL_REMOVE errors.

## Tests, build, and docs

- Tests: test_sync's off-loop spin, notify-spin, and no-herd tests
  count the lock's own condvar sleeps and wakeups instead of OS context
  switches; test_exec's Ex5 and test_sup's group-restart hook tests
  wait on the states they check instead of fixed sleeps; test_slab's
  stats_scale skips in coverage builds; test_lib_closure compares its
  symbol lists in the C locale.
- Coverage: scripts/coverage.sh runs every test past a failure and
  reports the failures, links the probe gates with the coverage
  runtime, and reports the DST-reachable public surface computed by
  scripts/dst_surface.py.  On the 2.3.0 candidate of 2026-11-20 that
  surface had 88.0% line, 70.1% branch, and 97.0% function coverage;
  it was not measured again on this tree.
- Portability: test_docs_abi, test_api_reach, test_man_coverage,
  test_api_discipline, test_alloc_overflow_sweep, and test_m4_man use
  POSIX grep patterns and nm formats.  On illumos the last three had
  matched nothing and printed OK without checking.  coro_uctx.c builds
  without a -Wclobbered warning on illumos, and the Windows
  build_msvc.bat gate fails when smoke.exe fails.
- Docs: an audit of the docs, man pages, and header comments against
  the code corrected signatures, return codes, counts, and behaviour
  that had drifted, and fixed a getting-started echo server that did
  not compile.  The doc-snippet gate now reads every page: a C block
  must be a marked fragment or a copy of a tested snippet file, and
  every output block a page shows must equal its program's real output.
  The correct two-message join is a tested snippet.
- Release scheme: development releases from `main` are named by date
  (vYYYY.MM.DD) and published as pre-releases; this ABI level, 2.3.0,
  ships first as one.  dist/version.in keeps the ABI level (the version
  macros, xtc_version_components, the SONAME, and the shared library's
  file name), and a release's dist/release.in gives its name to
  XTC_VERSION_STRING, xtc_version_string, and pkg-config's Version.
  dist/release_name.sh holds the rule for every build system.
  compat-check.sh takes the previous release of the tree's own line as
  N-1: the previous development release (or v2.2.0) on `main`, the
  previous 2.1.N on the LTS.  The README carries a badge for the latest
  development release.

## Qualification

On the candidate before the release-scheme commits (libxtc f925db45,
examples c6236f1), on a 48-core x86-64 Linux host, these passed: make
check on the io_uring, epoll, and poll backends with gcc, a clang
-Werror build, ASan with UBSan, and TSan on seven tests; the API, C++
coexistence, and doc-snippet gates; abidiff and compat-check against
v2.2.0; make check-dst (93 tests), the 44 pinned DST seeds, the 71
planted bugs, and a 100,000-seed swarm; the sqlxtc suite on io_uring
and epoll, with clang, and under valgrind (146 of 146 clean), plus
test-latched and the 232-case test-compat; the rexis suite with one
and two loops; and a fresh clone's make check.  The sqlxtc suite under
ASan failed only the 5 tests with known leaks on the connection-kill
path.

On the same candidate, on other platforms: Windows ARM64 and x64 (MSVC,
IOCP) passed the build_msvc.bat gate and its munit subset; FreeBSD 15.1
(kqueue) passed make check and check-dst; illumos passed make check;
riscv64 natively passed every test once test_lib_closure was run in
the C locale, the fix this release carries, and a riscv64 cross build
compiled cleanly.  macOS was not run.

On a one-socket x86-64 bare-metal host, with the release-scheme commits
in (libxtc 99f75c74): perf-gate against v2.2.0 passed twice, with 0
regressions in 19 cells.  The io_uring release gate passed its hazard
pass but failed its plain pass on the rexis race described above.
After the test fix, on this release's tree, the plain pass passed
three times with 0 POLL_REMOVE errors, and the hazard pass passed.

On the release commit itself: make check (autotools) and meson test,
abidiff and compat-check against v2.2.0, and the amalgamation.
