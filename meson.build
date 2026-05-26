project('xtc', 'c',
  version : run_command('cat', meson.project_source_root() / 'dist' / 'version.in',
                       check : true).stdout().strip(),
  license : 'PostgreSQL',
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
# -----------------------------------------------------------------------------
inc = include_directories('src/inc')

xtc_lib = static_library('xtc',
  sources : [
    'src/xtc_version.c',
    'src/xtc_strerror.c',
  ],
  include_directories : inc,
  install : true,
)

install_headers('src/inc/xtc.h')

# -----------------------------------------------------------------------------
# Tests.
# -----------------------------------------------------------------------------
test_inc = include_directories('src/inc', 'test/m0')

munit_dep = static_library('munit', 'test/m0/munit.c',
  include_directories : test_inc)

test_version = executable('test_version',
  'test/m0/test_version.c',
  include_directories : test_inc,
  link_with : [xtc_lib, munit_dep])

test_header = executable('test_header',
  'test/m0/test_header.c',
  include_directories : test_inc,
  link_with : [xtc_lib, munit_dep])

test_errors = executable('test_errors',
  'test/m0/test_errors.c',
  include_directories : test_inc,
  link_with : [xtc_lib, munit_dep])

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
