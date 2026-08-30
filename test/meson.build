# test/meson.build -- the C munit suite (dist/Makefile.in TESTS_C), wired
# into `meson test` at parity with the autotools `make check` C tier.
#
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# Each test links the static libxtc + its per-milestone munit.c (a symlinked
# copy in every test/mN dir), exactly as Makefile.in builds TESTS_C.  A test
# that returns 77 is a runtime skip (munit / plain-main convention); meson's
# test protocol maps exit 77 to SKIP via should_fail is not right, so we mark
# 77 as the skip code through the 'exit_codes' handling below.
#
# NOT wired here (run via autotools `make check`): the shell-gate tests
# (TESTS_SH), the PBT tier (TESTS_PBT, needs --with-hegel), and the DST/sim
# tier (check-dst, a separate sim-backend build).  The C munit suite is the
# parity target for meson.

build_root_inc = include_directories('.', '..')

# --------------------------------------------------------------------------
# The common case: test_<name>.c in test/<dir>/, linked with test/<dir>/munit.c
# and -I test/<dir>.  Encoded as [name, dir].
# --------------------------------------------------------------------------
munit_tests = [
  # M0
  ['version', 'm0'], ['header', 'm0'], ['errors', 'm0'],
  # M1
  ['atomic', 'm1'], ['alloc', 'm1'], ['sharp_edges', 'm1'], ['time', 'm1'],
  ['errno', 'm1'], ['cpu', 'm1'], ['cpu_cgroup', 'm1'], ['tuning', 'm1'],
  ['crypto', 'm1'], ['fs', 'm1'], ['dio', 'm1'], ['pkey', 'm1'],
  ['thread', 'm1'], ['tls', 'm1'], ['mutex', 'm1'],
  # M2
  ['bdev', 'm2'], ['io_lifecycle', 'm2'], ['io_fault_inject', 'm2'], ['io_common_edge', 'm2'],
  ['net', 'm2'], ['net_frame', 'm2'], ['net_udp', 'm2'],
  # M3
  ['loop', 'm3'], ['task', 'm3'], ['waker', 'm3'], ['timer', 'm3'],
  # M4
  ['async', 'm4'], ['aio', 'm4'], ['fctx', 'm4'], ['stack_guard', 'm4'],
  # M5
  ['cross_wake', 'm5'], ['deque', 'm5'], ['steal', 'm5'],
  # sim
  ['sim_rng', 'sim'],
  # M7
  ['chan', 'm7'], ['chan_mpmc_bcast', 'm7'], ['future', 'm7'],
  # M8
  ['proc', 'm8'], ['proc_wait_fd', 'm8'], ['recv_correlate', 'm8'],
  ['proc_link_race', 'm8'],
  # M9
  ['sync', 'm9'], ['amutex_xloop', 'm9'], ['dio_sched', 'm9'],
  ['iosched', 'm9'], ['blocking', 'm9'], ['alloc_audit', 'm9'],
  # M10
  ['sup', 'm10'], ['reg', 'm10'], ['svr', 'm10'], ['svr_edge', 'm10'],
  ['fsm', 'm10'], ['saga', 'm10'], ['pg', 'm10'], ['pool', 'm10'],
  ['stream', 'm10'], ['credit', 'm10'], ['xproc', 'm10'], ['app', 'm10'],
  ['osproc', 'm10'], ['isolated', 'm10'],
  # M11
  ['mctx', 'm11'], ['slab', 'm11'], ['slab_shm', 'm11'],
  # M12
  ['observability', 'm12'], ['runtime', 'm12'], ['stats', 'm12'],
  ['tail', 'm12'], ['dump', 'm12'],
  # M13
  ['rcu', 'm13'], ['chash', 'm13'], ['cskip', 'm13'], ['prob', 'm13'],
  ['accel', 'm13'], ['lrlock', 'm13'], ['lwlock', 'm13'], ['lockmgr', 'm13'],
  # M14
  ['preempt', 'm14'], ['preempt_p1', 'm14'], ['preempt_p2', 'm14'],
  ['unsafe_depth', 'm14'], ['stack_reclaim', 'm14'], ['launch', 'm14'],
  # coverage
  ['coverage_pump', 'coverage'], ['fault_inject', 'coverage'],
  # OTP port
  ['otp_proc_lib', 'otp'], ['otp_gen_server', 'otp'],
  ['otp_gen_server_phase2', 'otp'], ['otp_supervisor', 'otp'],
  # M18 TLS
  ['tls_basic', 'm18'], ['tls_server', 'm18'], ['tls_client', 'm18'],
]

# Tests that additionally need -I test/include (io backends' pipe compat).
io_include_tests = {
  'io_register'    : 'm2',
  'io_events'      : 'm2',
  'io_wakeup'      : 'm2',
  'io_integration' : 'm3',
  'exec'           : 'm5',
}

foreach spec : munit_tests
  name = spec[0]
  d = spec[1]
  test('@0@/@1@'.format(d, name),
    executable('test_' + name,
      files('@0@/test_@1@.c'.format(d, name), '@0@/munit.c'.format(d)),
      include_directories : [inc, build_root_inc, include_directories(d)],
      c_args : xtc_c_args,
      link_with : xtc_static,
      dependencies : lib_deps),
    timeout : 120)
endforeach

foreach name, d : io_include_tests
  test('@0@/@1@'.format(d, name),
    executable('test_' + name,
      files('@0@/test_@1@.c'.format(d, name), '@0@/munit.c'.format(d)),
      include_directories : [inc, build_root_inc, include_directories(d, 'include')],
      c_args : xtc_c_args,
      link_with : xtc_static,
      dependencies : lib_deps),
    timeout : 120)
endforeach

# --------------------------------------------------------------------------
# test_inject_races: concurrency/ source + m5 munit, -I test/m5.
# --------------------------------------------------------------------------
test('concurrency/inject_races',
  executable('test_inject_races',
    files('concurrency/test_inject_races.c', 'm5/munit.c'),
    include_directories : [inc, build_root_inc, include_directories('m5')],
    c_args : xtc_c_args,
    link_with : xtc_static,
    dependencies : lib_deps),
  timeout : 120)

# --------------------------------------------------------------------------
# Plain-main tests (no munit): return 0 pass / non-zero fail / 77 skip.
# --------------------------------------------------------------------------
plain_tests = [
  ['m1/thread_sigmask',                     'm1/test_thread_sigmask.c'],
  ['concurrency/proc_wake_crossthread',     'concurrency/test_proc_wake_crossthread.c'],
  ['concurrency/proc_table_stress',         'concurrency/test_proc_table_stress.c'],
  ['concurrency/eager_rebalance',           'concurrency/test_eager_rebalance.c'],
  ['concurrency/wake_after_migration',      'concurrency/test_wake_after_migration.c'],
  ['tnt/tnt',                               'tnt/test_tnt.c'],
]
foreach spec : plain_tests
  tname = spec[0]
  src = spec[1]
  exe_name = 'test_' + tname.split('/')[1]
  test(tname,
    executable(exe_name,
      files(src),
      include_directories : [inc, build_root_inc],
      c_args : xtc_c_args,
      link_with : xtc_static,
      dependencies : lib_deps),
    timeout : 120)
endforeach

# --------------------------------------------------------------------------
# test_backtrace: needs -rdynamic so the test's own functions symbolize.
# --------------------------------------------------------------------------
test('m12/backtrace',
  executable('test_backtrace',
    files('m12/test_backtrace.c', 'm12/munit.c'),
    include_directories : [inc, build_root_inc, include_directories('m12')],
    c_args : xtc_c_args,
    link_args : ['-rdynamic'],
    link_with : xtc_static,
    dependencies : lib_deps),
  timeout : 120)

# --------------------------------------------------------------------------
# test_pgmock_smoke: m16 test + the examples/09_pgmock mock sources,
# -I examples/09_pgmock.  Plain main (no munit).
# --------------------------------------------------------------------------
test('m16/pgmock_smoke',
  executable('test_pgmock_smoke',
    files('m16/test_pgmock_smoke.c',
          '../examples/09_pgmock/listener.c',
          '../examples/09_pgmock/backend.c',
          '../examples/09_pgmock/pg_latch.c'),
    include_directories : [inc, build_root_inc,
                           include_directories('../examples/09_pgmock')],
    c_args : xtc_c_args,
    link_with : xtc_static,
    dependencies : lib_deps),
  timeout : 120)
