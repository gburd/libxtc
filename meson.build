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

# =============================================================================
# meson build -- AT PARITY with the autotools build (dist/configure +
# dist/Makefile.in).  Both build systems compile the same LIB_SRCS into the
# same static (and, with -Dshared=true, shared) libxtc, run the same C munit
# suite, and install the same public headers / man pages / pkg-config file.
#
# configure.ac / dist/Makefile.in remain the reference for WHAT is built;
# this file mirrors their source list and their backend/coro/TLS selection.
# The io/coro/asm sources all self-guard on the XTC_* config macros (they
# compile to empty units when their backend/arch is not selected), exactly
# as the autotools build compiles the whole list -- so meson does the same
# and lets xtc_config.h pick the active code.  Only tls_<backend>.c is
# selected by name (one per build), matching Makefile.in's TLS_OBJS.
# =============================================================================

cc = meson.get_compiler('c')
host_cpu = host_machine.cpu_family()
host_sys = host_machine.system()

# -----------------------------------------------------------------------------
# Configuration header.  Emits the SAME defines configure.ac AC_DEFINEs, from
# the same detection logic.  The source #ifdefs on these (grep src/ for
# XTC_IO_BACKEND_*, XTC_TLS_BACKEND_*, XTC_HAVE_*).
# -----------------------------------------------------------------------------
cdata = configuration_data()

ver = meson.project_version()
ver_parts = ver.split('-')[0].split('.')
cdata.set_quoted('XTC_VERSION_STRING', ver)
cdata.set('XTC_VERSION_MAJOR', ver_parts[0].to_int())
cdata.set('XTC_VERSION_MINOR', ver_parts[1].to_int())
cdata.set('XTC_VERSION_PATCH', ver_parts[2].to_int())

if get_option('diagnostic')
  cdata.set('XTC_DIAGNOSTIC', 1)
endif

# -- pthread / platform base libs -------------------------------------------
thread_dep = dependency('threads')
platform_deps = [thread_dep]

# librt / libdl best-effort (clock_gettime, timer_create, shm_open, dladdr).
# On modern glibc these live in libc; find them only where a separate lib
# exists (mirrors configure.ac AC_SEARCH_LIBS).
foreach l : ['rt', 'dl']
  dep = cc.find_library(l, required : false)
  if dep.found()
    platform_deps += dep
  endif
endforeach

# -- backtrace substrate (execinfo / builtin unwind / libunwind) -------------
# Priority mirrors configure.ac: execinfo wins; else builtin _Unwind_Backtrace;
# else libunwind (only probed when the two above are absent).
have_execinfo = cc.has_header('execinfo.h') and cc.has_function('backtrace', prefix : '#include <execinfo.h>')
if have_execinfo
  cdata.set('XTC_HAVE_EXECINFO', 1)
endif

have_builtin_unwind = cc.links('''#include <unwind.h>
  static _Unwind_Reason_Code cb(struct _Unwind_Context *c, void *a)
  { (void)a; (void)_Unwind_GetIP(c); return _URC_NO_REASON; }
  int main(void){ (void)_Unwind_Backtrace(cb, 0); return 0; }''',
  name : '_Unwind_Backtrace')

use_libunwind = false
use_builtin_unwind = false
if not have_execinfo
  if have_builtin_unwind
    use_builtin_unwind = true
  else
    libunwind_dep = dependency('libunwind', required : false)
    if libunwind_dep.found()
      use_libunwind = true
      platform_deps += libunwind_dep
    endif
  endif
endif
if use_libunwind
  cdata.set('XTC_HAVE_LIBUNWIND', 1)
endif
if use_builtin_unwind
  cdata.set('XTC_HAVE_BUILTIN_UNWIND', 1)
endif
if use_libunwind or use_builtin_unwind
  if cc.has_function('dladdr', prefix : '#define _GNU_SOURCE\n#include <dlfcn.h>')
    cdata.set('XTC_HAVE_DLADDR', 1)
  endif
endif

# -- ucontext (default coroutine substrate; musl omits it) -------------------
if cc.has_function('swapcontext', prefix : '#include <ucontext.h>')
  cdata.set('XTC_HAVE_UCONTEXT', 1)
endif

# -- liburing detection (for the uring io backend) ---------------------------
uring_dep = dependency('', required : false)
have_uring = false
liburing_path = get_option('liburing')
if liburing_path != ''
  # Explicit prefix: honour its pkgconfig, else header+lib under it.
  uring_dep = dependency('liburing', required : false,
    dirs : [liburing_path / 'lib' / 'pkgconfig'])
  if not uring_dep.found()
    uring_dep = declare_dependency(
      include_directories : include_directories(liburing_path / 'include'),
      dependencies : cc.find_library('uring', dirs : [liburing_path / 'lib']))
  endif
  have_uring = true
else
  uring_dep = dependency('liburing', required : false)
  if not uring_dep.found()
    if cc.has_header('liburing.h')
      luring = cc.find_library('uring', required : false, has_headers : ['liburing.h'])
      if luring.found()
        uring_dep = luring
        have_uring = true
      endif
    endif
  else
    have_uring = true
  endif
endif

have_epoll = cc.has_header('sys/epoll.h')
have_kqueue = cc.has_header('sys/event.h')

# -- I/O backend selection (mirrors configure.ac case logic) -----------------
io_backend = get_option('io-backend')
if io_backend == 'auto'
  if host_sys == 'windows'
    io_backend = 'iocp'
  elif host_sys == 'sunos'
    io_backend = 'solaris'
  elif host_sys == 'aix'
    io_backend = 'aix'
  elif have_uring
    io_backend = 'uring'
  elif have_epoll
    io_backend = 'epoll'
  elif have_kqueue
    io_backend = 'kqueue'
  else
    io_backend = 'poll'
  endif
else
  if io_backend == 'uring' and not have_uring
    error('io-backend=uring requires liburing (set -Dliburing=PATH)')
  elif io_backend == 'epoll' and not have_epoll
    error('io-backend=epoll requires <sys/epoll.h>')
  elif io_backend == 'kqueue' and not have_kqueue
    error('io-backend=kqueue requires <sys/event.h>')
  endif
endif

io_macro = {
  'epoll'   : 'XTC_IO_BACKEND_EPOLL',
  'poll'    : 'XTC_IO_BACKEND_POLL',
  'uring'   : 'XTC_IO_BACKEND_URING',
  'kqueue'  : 'XTC_IO_BACKEND_KQUEUE',
  'iocp'    : 'XTC_IO_BACKEND_IOCP',
  'solaris' : 'XTC_IO_BACKEND_SOLARIS',
  'aix'     : 'XTC_IO_BACKEND_AIX',
  'select'  : 'XTC_IO_BACKEND_SELECT',
  'sim'     : 'XTC_IO_BACKEND_SIM',
}
cdata.set(io_macro[io_backend], 1)

lib_deps = platform_deps
if io_backend == 'uring'
  lib_deps += uring_dep
endif

# Windows / Solaris extra platform libs (mirrors XTC_PLATFORM_LIBS).
if host_sys == 'windows'
  foreach l : ['ws2_32', 'ntdll', 'dbghelp']
    lib_deps += cc.find_library(l, required : false)
  endforeach
elif host_sys == 'sunos'
  foreach l : ['socket', 'nsl']
    lib_deps += cc.find_library(l, required : false)
  endforeach
endif

# -- attached-compute (accel) ------------------------------------------------
accel_opt = get_option('accel')
have_accel = false
if accel_opt != 'no'
  have_accel = cc.has_header('linux/dma-buf.h') or cc.has_header('drm/drm.h') or cc.has_header('libdrm/drm.h')
  if accel_opt == 'yes' and not have_accel
    error('accel=yes: no Linux DRM/accel uapi header found')
  endif
endif
if have_accel
  cdata.set('XTC_HAVE_ACCEL', 1)
  libdrm_dep = dependency('libdrm', required : false)
  if libdrm_dep.found()
    cdata.set('XTC_HAVE_LIBDRM', 1)
    lib_deps += libdrm_dep
  endif
endif

# -- TLS backend selection (mirrors configure.ac) ----------------------------
tls_opt = get_option('tls')
tls_backend = 'none'
tls_dep = dependency('', required : false)

if tls_opt == 'auto'
  ossl = dependency('openssl', required : false)
  if ossl.found()
    tls_backend = 'openssl'
    tls_dep = ossl
  elif cc.has_header('openssl/ssl.h') and cc.find_library('ssl', required : false).found()
    tls_backend = 'openssl'
    tls_dep = declare_dependency(dependencies : [
      cc.find_library('ssl'), cc.find_library('crypto')])
  endif
elif tls_opt == 'openssl' or tls_opt == 'libressl' or tls_opt == 'boringssl'
  ossl = dependency('openssl', required : false)
  if ossl.found()
    tls_dep = ossl
  else
    tls_dep = declare_dependency(dependencies : [
      cc.find_library('ssl', has_headers : ['openssl/ssl.h']),
      cc.find_library('crypto')])
  endif
  tls_backend = 'openssl'  # boringssl/libressl share tls_openssl.c
elif tls_opt == 'mbedtls'
  md = dependency('mbedtls', required : false)
  if md.found()
    tls_dep = declare_dependency(dependencies : [md,
      cc.find_library('mbedx509'), cc.find_library('mbedcrypto')])
  else
    tls_dep = declare_dependency(dependencies : [
      cc.find_library('mbedtls', has_headers : ['mbedtls/ssl.h']),
      cc.find_library('mbedx509'), cc.find_library('mbedcrypto')])
  endif
  tls_backend = 'mbedtls'
elif tls_opt == 'gnutls'
  gt = dependency('gnutls', required : false)
  tls_dep = gt.found() ? gt : declare_dependency(dependencies :
    cc.find_library('gnutls', has_headers : ['gnutls/gnutls.h']))
  tls_backend = 'gnutls'
elif tls_opt == 'wolfssl'
  ws = dependency('wolfssl', required : false)
  tls_dep = ws.found() ? ws : declare_dependency(dependencies :
    cc.find_library('wolfssl', has_headers : ['wolfssl/ssl.h']))
  tls_backend = 'wolfssl'
elif tls_opt == 'schannel'
  if host_sys != 'windows'
    error('tls=schannel is Windows-only')
  endif
  tls_dep = declare_dependency(dependencies : [
    cc.find_library('secur32'), cc.find_library('crypt32')])
  tls_backend = 'schannel'
endif

tls_macro = {
  'openssl'  : 'XTC_TLS_BACKEND_OPENSSL',
  'mbedtls'  : 'XTC_TLS_BACKEND_MBEDTLS',
  'gnutls'   : 'XTC_TLS_BACKEND_GNUTLS',
  'wolfssl'  : 'XTC_TLS_BACKEND_WOLFSSL',
  'schannel' : 'XTC_TLS_BACKEND_SCHANNEL',
  'none'     : 'XTC_TLS_BACKEND_NONE',
}
cdata.set(tls_macro[tls_backend], 1)
if tls_backend != 'none'
  lib_deps += tls_dep
endif

xtc_config_h = configure_file(
  output : 'xtc_config.h',
  configuration : cdata,
)

message('xtc meson: io-backend=' + io_backend + ' tls=' + tls_backend
  + ' accel=' + (have_accel ? 'yes' : 'no'))

# -----------------------------------------------------------------------------
# Library sources.  Mirrors dist/Makefile.in LIB_SRCS.  All io/coro/asm files
# are compiled (they self-guard on the config macros); one tls_<backend>.c is
# selected by name via TLS_OBJS.
# -----------------------------------------------------------------------------
inc = include_directories('src/inc')

lib_sources = files(
  'src/xtc_version.c',
  'src/xtc_strerror.c',
  'src/os/os_alloc.c',
  'src/os/os_env.c',
  'src/os/os_errno.c',
  'src/os/os_rand.c',
  'src/os/os_str.c',
  'src/os/os_time.c',
  'src/os/os_thread.c',
  'src/os/os_tls.c',
  'src/os/os_mutex.c',
  'src/os/os_cpu.c',
  'src/os/os_tuning.c',
  'src/os/os_crypto.c',
  'src/os/os_pkey.c',
  'src/os/os_backtrace.c',
  'src/os/os_file.c',
  'src/io/io_common.c',
  'src/io/io_bdev.c',
  'src/io/io_net.c',
  'src/io/io_poll.c',
  'src/io/io_epoll.c',
  'src/io/io_uring.c',
  'src/io/io_kqueue.c',
  'src/io/io_iocp.c',
  'src/io/io_solaris.c',
  'src/io/io_aix.c',
  'src/io/io_select.c',
  'src/io/io_sim.c',
  'src/evt/loop.c',
  'src/evt/task.c',
  'src/evt/timer.c',
  'src/evt/coro_uctx.c',
  'src/evt/coro_fctx.c',
  'src/evt/coro_winfiber.c',
  'src/evt/exec.c',
  'src/evt/sim.c',
  'src/ptc/res.c',
  'src/ptc/chan.c',
  'src/ptc/proc.c',
  'src/ptc/sync.c',
  'src/ptc/future.c',
  'src/ptc/dispatch.c',
  'src/ptc/log.c',
  'src/ptc/cfg.c',
  'src/ptc/inject.c',
  'src/ptc/pdict.c',
  'src/ptc/preempt.c',
  'src/ptc/stats.c',
  'src/ptc/tail.c',
  'src/ptc/slab.c',
  'src/ptc/slab_reaper.c',
  'src/ptc/blocking.c',
  'src/ptc/accel.c',
  'src/ptc/alloc_audit.c',
  'src/ptc/mctx.c',
  'src/ptc/rcu.c',
  'src/ptc/chash.c',
  'src/ptc/cskip.c',
  'src/ptc/prob.c',
  'src/ptc/dio_sched.c',
  'src/ptc/iosched.c',
  'src/ptc/dump.c',
  'src/ptc/aio.c',
  'src/orc/sup.c',
  'src/orc/reg.c',
  'src/orc/svr.c',
  'src/orc/fsm.c',
  'src/orc/saga.c',
  'src/orc/pg.c',
  'src/orc/pool.c',
  'src/orc/stream.c',
  'src/orc/credit.c',
  'src/orc/xproc.c',
  'src/orc/launch.c',
  'src/orc/app.c',
  'src/orc/tnt.c',
  'src/orc/osproc.c',
  'src/ptc/lock_lr.c',
  'src/ptc/lock_lw.c',
  'src/ptc/lock_mgr.c',
  # Coroutine + preempt assembly.  Each .S self-guards on the target arch,
  # so the whole set is compiled and the non-matching ones are empty.
  'src/os/asm/fctx_x86_64_sysv.S',
  'src/os/asm/fctx_x86_64_ms_pe.S',
  'src/os/asm/fctx_aarch64_aapcs.S',
  'src/os/asm/fctx_aarch64_aapcs_macho.S',
  'src/os/asm/fctx_aarch64_ms_pe.S',
  'src/os/asm/fctx_ppc64le_elfv2.S',
  'src/os/asm/fctx_riscv64_lp64d.S',
  'src/os/asm/fctx_arm_aapcs.S',
  'src/os/asm/fctx_s390x_zabi.S',
  'src/os/asm/fctx_sparc64_sysv.S',
  'src/os/asm/preempt_trampoline_x86_64.S',
  'src/os/asm/preempt_trampoline_aarch64.S',
)

# The one selected TLS backend source (mirrors TLS_OBJS = tls_@BACKEND@.o).
lib_sources += files('src/io/tls_' + tls_backend + '.c')

# CFLAGS: match dist/Makefile.in.  -DXTC_BUILDING_DLL always on (no-op off
# Windows).  -D_DEFAULT_SOURCE is added globally by configure so public
# headers see POSIX under -std=c11.  -Wpedantic to match the autotools warn
# set; the .S files are excluded from -Wpedantic by the assembler anyway.
xtc_c_args = ['-DXTC_BUILDING_DLL', '-D_DEFAULT_SOURCE', '-Wpedantic']

build_shared = get_option('shared')

# ELF version script (dist/libxtc.map) restricts the shared-lib exported ABI
# to the public xtc_* / __xtc_* surface, hiding the internal __os_* layer --
# mirrors XTC_SHLIB_LDFLAGS' -Wl,--version-script in dist/configure.ac.  Only
# the ELF (GNU ld) style is wired here; Darwin/Windows use their own paths in
# the autotools build and are not meson targets on this workstream.
xtc_shared_link_args = []
if build_shared and host_sys not in ['darwin', 'windows']
  xtc_shared_link_args += '-Wl,--version-script=@0@'.format(
    meson.project_source_root() / 'dist' / 'libxtc.map')
endif

if build_shared
  xtc_lib = both_libraries('xtc',
    sources : [lib_sources, xtc_config_h],
    include_directories : inc,
    c_args : xtc_c_args,
    link_args : xtc_shared_link_args,
    dependencies : lib_deps,
    version : meson.project_version(),
    soversion : ver_parts[0],
    install : true,
  )
  xtc_static = xtc_lib.get_static_lib()
else
  xtc_lib = static_library('xtc',
    sources : [lib_sources, xtc_config_h],
    include_directories : inc,
    c_args : xtc_c_args,
    dependencies : lib_deps,
    install : true,
  )
  xtc_static = xtc_lib
endif

# -----------------------------------------------------------------------------
# Public headers -- full LIB_HDRS_PUBLIC set from dist/Makefile.in.
# -----------------------------------------------------------------------------
install_headers(
  'src/inc/xtc.h',
  'src/inc/xtc_export.h',
  'src/inc/xtc_runtime.h',
  'src/inc/xtc_aio.h',
  'src/inc/xtc_alloc_audit.h',
  'src/inc/xtc_app.h',
  'src/inc/xtc_async.h',
  'src/inc/xtc_bdev.h',
  'src/inc/xtc_blocking.h',
  'src/inc/xtc_accel.h',
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
  'src/inc/xtc_chash.h',
  'src/inc/xtc_cskip.h',
  'src/inc/xtc_prob.h',
  'src/inc/xtc_reg.h',
  'src/inc/xtc_res.h',
  'src/inc/xtc_slab.h',
  'src/inc/xtc_stats.h',
  'src/inc/xtc_tail.h',
  'src/inc/xtc_svr.h',
  'src/inc/xtc_fsm.h',
  'src/inc/xtc_saga.h',
  'src/inc/xtc_pg.h',
  'src/inc/xtc_pool.h',
  'src/inc/xtc_stream.h',
  'src/inc/xtc_credit.h',
  'src/inc/xtc_xproc.h',
  'src/inc/xtc_launch.h',
  'src/inc/xtc_sync.h',
  'src/inc/xtc_future.h',
  'src/inc/xtc_dispatch.h',
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

# -----------------------------------------------------------------------------
# Man pages.  Install the whole man3/ + man7/ set (mirrors `make install`).
# -----------------------------------------------------------------------------
install_subdir('man/man3', install_dir : 'share/man', strip_directory : false)
install_subdir('man/man7', install_dir : 'share/man', strip_directory : false)

# -----------------------------------------------------------------------------
# Tests -- the C munit suite (TESTS_C).  Shell gates / PBT / sim are run via
# the autotools `make check`; see test/meson.build for what is wired here.
# -----------------------------------------------------------------------------
subdir('test')
