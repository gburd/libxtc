project('xtc', 'c',
  version : run_command('cat', meson.project_source_root() / 'dist' / 'version.in',
                       check : true).stdout().strip(),
  license : 'ISC',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'b_ndebug=if-release',
  ],
  meson_version : '>=0.61.0',
)

# -----------------------------------------------------------------------------
# Configuration header.  Same shape as the autoconf-generated xtc_config.h.
# -----------------------------------------------------------------------------
cdata = configuration_data()

ver = meson.project_version()
ver_parts = ver.split('-')[0].split('.')
cdata.set_quoted('XTC_VERSION_STRING', ver)
cdata.set('XTC_VERSION_MAJOR', ver_parts[0].to_int())
cdata.set('XTC_VERSION_MINOR', ver_parts[1].to_int())
cdata.set('XTC_VERSION_PATCH', ver_parts[2].to_int())

configure_file(
  output : 'xtc_config.h',
  configuration : cdata,
)

# -----------------------------------------------------------------------------
# Library.
#
# meson installs a pkg-config xtc.pc and (with -Dshared=true) a versioned
# shared object alongside the static archive.  The autoconf build is the
# reference; this mirrors its installed file set for the M0 subset that
# meson currently compiles.
# -----------------------------------------------------------------------------
inc = include_directories('src/inc')

lib_sources = [
  'src/xtc_version.c',
  'src/xtc_strerror.c',
  'src/os/os_alloc.c',        # xtc_free (in xtc_strerror.c) calls __os_free
  'src/os/os_env.c',          # xtc_env_get/_set (in xtc_strerror.c)
  'src/os/os_rand.c',         # xtc_rand_u64/_seed (in xtc_strerror.c)
  'src/os/os_str.c',          # xtc_strlcpy/_strlcat (in xtc_strerror.c)
  'src/os/os_time.c',         # xtc_clock_mono/_real/_sleep_ns (in xtc_strerror.c)
  'src/os/os_unsafe_stub.c',  # no-op __xtc_unsafe_* (preempt.c not in the M0 lib)
]

build_shared = get_option('shared')

if build_shared
  xtc_lib = both_libraries('xtc',
    sources : lib_sources,
    include_directories : inc,
    version : meson.project_version(),
    soversion : ver_parts[0],
    install : true,
  )
else
  xtc_lib = static_library('xtc',
    sources : lib_sources,
    include_directories : inc,
    install : true,
  )
endif

# Public headers.  xtc.h is the umbrella; the per-subsystem xtc_*.h
# carry the functional API.  Kept in step with dist/Makefile.in's
# LIB_HDRS_PUBLIC and the surface in dist/mkamalgamation.py.
install_headers(
  'src/inc/xtc.h',
  'src/inc/xtc_runtime.h',
  'src/inc/xtc_aio.h',
  'src/inc/xtc_alloc_audit.h',
  'src/inc/xtc_app.h',
  'src/inc/xtc_async.h',
  'src/inc/xtc_bdev.h',
  'src/inc/xtc_blocking.h',
  'src/inc/xtc_cfg.h',
  'src/inc/xtc_chan.h',
  'src/inc/xtc_dio_sched.h',
  'src/inc/xtc_dump.h',
  'src/inc/xtc_exec.h',
  'src/inc/xtc_fs.h',
  'src/inc/xtc_inject.h',
  'src/inc/xtc_inspect.h',
  'src/inc/xtc_io.h',
  'src/inc/xtc_iosched.h',
  'src/inc/xtc_lockmgr.h',
  'src/inc/xtc_log.h',
  'src/inc/xtc_loop.h',
  'src/inc/xtc_lrlock.h',
  'src/inc/xtc_lwlock.h',
  'src/inc/xtc_mctx.h',
  'src/inc/xtc_net.h',
  'src/inc/xtc_orc.h',
  'src/inc/xtc_osproc.h',
  'src/inc/xtc_pdict.h',
  'src/inc/xtc_preempt.h',
  'src/inc/xtc_pkey.h',
  'src/inc/xtc_proc.h',
  'src/inc/xtc_rcu.h',
  'src/inc/xtc_reg.h',
  'src/inc/xtc_res.h',
  'src/inc/xtc_slab.h',
  'src/inc/xtc_stats.h',
  'src/inc/xtc_svr.h',
  'src/inc/xtc_fsm.h',
  'src/inc/xtc_pg.h',
  'src/inc/xtc_sync.h',
  'src/inc/xtc_tailcall.h',
  'src/inc/xtc_tls.h',
  'src/inc/xtc_tnt.h',
  'src/inc/xtc_trace.h',
)

# pkg-config metadata.  Mirrors dist/xtc.pc.in.
pkg = import('pkgconfig')
pkg.generate(
  name : 'xtc',
  description : 'High-performance async/concurrency runtime for C',
  url : 'https://codeberg.org/gregburd/libxtc',
  version : meson.project_version(),
  libraries : xtc_lib,
  subdirs : '.',
)

if build_shared
  xtc_link_lib = xtc_lib.get_shared_lib()
else
  xtc_link_lib = xtc_lib
endif

# -----------------------------------------------------------------------------
# Tests.
# -----------------------------------------------------------------------------
test_inc = include_directories('src/inc', 'test/m0')

munit_dep = static_library('munit', 'test/m0/munit.c',
  include_directories : test_inc)

test_version = executable('test_version',
  'test/m0/test_version.c',
  include_directories : test_inc,
  link_with : [xtc_link_lib, munit_dep])

test_header = executable('test_header',
  'test/m0/test_header.c',
  include_directories : test_inc,
  link_with : [xtc_link_lib, munit_dep])

test_errors = executable('test_errors',
  'test/m0/test_errors.c',
  include_directories : test_inc,
  link_with : [xtc_link_lib, munit_dep])

test('m0/version', test_version)
test('m0/header',  test_header)
test('m0/errors',  test_errors)

# Shell-script claim tests.
shell_tests = [
  ['m0/oos_enforced',     'test/m0/test_oos_enforced.sh'],
  ['m0/config_header',    'test/m0/test_config_header.sh'],
  ['m0/symbols',          'test/m0/test_symbols.sh'],
  ['m0/readme_build',     'test/m0/test_readme_build.sh'],
  ['m0/docs_arch',        'test/m0/test_docs_arch.sh'],
  ['m0/docs_abi',         'test/m0/test_docs_abi.sh'],
  ['m0/man_coverage',     'test/m0/test_man_coverage.sh'],
  ['m0/man_lint',         'test/m0/test_man_lint.sh'],
  ['m0/man_signatures',   'test/m0/test_man_signatures.sh'],
  ['dist/s_all',          'test/dist/test_s_all.sh'],
  ['dist/s_include',      'test/dist/test_s_include.sh'],
  ['dist/s_include_idem', 'test/dist/test_s_include_idempotent.sh'],
  ['dist/shellcheck',     'test/dist/test_shellcheck.sh'],
  ['dist/s_perm',         'test/dist/test_s_perm.sh'],
]

foreach t : shell_tests
  test(t[0], find_program(t[1]),
       env : [
         'XTC_SRC_DIR=' + meson.project_source_root(),
         'XTC_BUILD_DIR=' + meson.project_build_root(),
       ],
       depends : [xtc_lib])
endforeach

# Install man pages.
install_data('man/man3/xtc_version_string.3',     install_dir : 'share/man/man3')
install_data('man/man3/xtc_version_components.3', install_dir : 'share/man/man3')
install_data('man/man3/xtc_strerror.3',           install_dir : 'share/man/man3')
install_data('man/man7/xtc.7',                    install_dir : 'share/man/man7')
install_data('man/man7/xtc-build.7',              install_dir : 'share/man/man7')
