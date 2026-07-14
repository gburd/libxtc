@echo off
rem ---------------------------------------------------------------------------
rem dist\build_msvc.bat
rem
rem   Build libxtc as a static library with the Microsoft toolchain
rem   (cl.exe + ml64.exe + lib.exe), build one smoke test, and
rem   best-effort build+run one real munit-suite test (test/m1/test_atomic.c)
rem   now that the harness's VLA-param macro (MUNIT_ARRAY_PARAM) has been
rem   fixed to standard C11 -- see step 5 below and docs/M_WINDOWS_MATRIX.md.
rem
rem   Run from a directory you want the objects in, inside a Visual
rem   Studio "x64 Native Tools" environment (or after calling
rem   vcvars64.bat), with %XTC_SRC% pointing at the source root:
rem
rem     set XTC_SRC=C:\scratch\xtc
rem     call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
rem     C:\scratch\xtc\dist\build_msvc.bat
rem
rem   Produces xtc.lib in the current directory.
rem ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

if "%XTC_SRC%"=="" set XTC_SRC=%~dp0..
echo XTC_SRC=%XTC_SRC%

set INC=/I"%XTC_SRC%\src\inc" /I"%XTC_SRC%\src\inc\compat" /I.
rem /experimental:c11atomics enables _Atomic on VS2022 17.5+ / VS2026.
rem /DXTC_BUILDING_DLL marks these objects as "I am the library" for
rem the XTC_API export macro (src/inc/xtc_export.h): it selects
rem __declspec(dllexport) on every PUBLIC prototype.  This build still
rem archives into a STATIC xtc.lib (lib.exe below, no link.exe /DLL
rem step) -- see docs/M_WINDOWS_MATRIX.md -- so the dllexport directive
rem is inert here (no DLL is produced to export from) but its presence
rem is what a follow-up DLL-producing build needs unchanged.  Wiring an
rem actual link.exe /DLL step, a generated import .lib, and pointing
rem smoke.c at it is NOT done in this pass (needs a Windows host to
rem build+run+verify); this only makes the export-macro plumbing that
rem such a follow-up would require already correct and in place.
set CFLAGS=/nologo /std:c11 /experimental:c11atomics /W3 /WX /O2 /MT /D_WIN32 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DXTC_BUILDING_DLL %INC%

rem --- 1. Assemble the fcontext primitives with the right assembler
rem        for the target arch.  ml64 assembles the x86-64 MASM file;
rem        armasm64 assembles the ARM64 file.  VSCMD_ARG_TGT_ARCH is set
rem        by vcvarsall.bat / the Native Tools prompt (x64 or arm64). ---
if /I "%VSCMD_ARG_TGT_ARCH%"=="arm64" (
  echo [1/5] armasm64 fctx_aarch64_ms_pe.asm
  armasm64 -nologo -o fctx_aarch64_ms_pe.obj "%XTC_SRC%\src\os\asm\fctx_aarch64_ms_pe.asm"
) else (
  echo [1/5] ml64 fctx_x86_64_ms_pe.asm
  ml64 /nologo /c /Fo fctx_x86_64_ms_pe.obj "%XTC_SRC%\src\os\asm\fctx_x86_64_ms_pe.asm"
)
if errorlevel 1 goto :fail

rem --- 2. Compile the C sources.  Platform-specific backends
rem        (io_epoll/io_kqueue/io_solaris/io_uring/io_aix, coro_uctx)
rem        self-guard to empty translation units on Windows. ---
echo [2/5] cl compiling sources
set SRCS=
for %%D in (os io evt ptc orc) do (
  for %%F in ("%XTC_SRC%\src\%%D\*.c") do (
    rem os_unsafe_stub.c is a meson-M0-only no-op; the full MSVC build
    rem links preempt.c which provides the real __xtc_unsafe_* -- skip
    rem the stub to avoid a duplicate-symbol (LNK4006).
    if /I not "%%~nxF"=="os_unsafe_stub.c" set SRCS=!SRCS! "%%F"
  )
)
set SRCS=!SRCS! "%XTC_SRC%\src\xtc_version.c" "%XTC_SRC%\src\xtc_strerror.c"

cl %CFLAGS% /c !SRCS!
if errorlevel 1 goto :fail

rem --- 3. Archive into xtc.lib. ---
echo [3/5] lib xtc.lib
lib /nologo /OUT:xtc.lib *.obj
if errorlevel 1 goto :fail

rem --- 4. Build the MSVC smoke test (munit uses GCC-isms cl rejects;
rem        this standalone test exercises the Win32 paths directly). ---
echo [4/5] cl smoke.exe
cl %CFLAGS% /Fe:smoke.exe ^
   "%XTC_SRC%\test\msvc\smoke.c" ^
   xtc.lib ws2_32.lib ntdll.lib dbghelp.lib
if errorlevel 1 goto :fail

echo.
echo BUILD OK: xtc.lib + smoke.exe
smoke.exe

rem --- 5. Build + run a curated set of REAL munit-suite tests under
rem        cl.exe, now that MUNIT_ARRAY_PARAM no longer expands to a
rem        VLA-typed array parameter (test/m0/munit.h) -- that was the
rem        GCC-ism blocking cl.exe.  The set is the POSIX-CLEAN munit
rem        tests: they exercise only the public xtc_* API + the harness,
rem        with no direct pthread.h / unistd.h / sys-header use, so they
rem        are the portion of the suite that compiles under UCRT.
rem        (Tests that use POSIX threads/sockets directly stay Linux/CI-
rem        validated; porting those is the separate Clang64 work.
rem        test_tls is excluded: no TLS backend is wired into the MSVC
rem        build.)
rem
rem        This is now a HARD GATE: confirmed all-pass on Windows CI
rem        (16/16), so any build/run failure in the set fails the job.
rem        See docs/M_WINDOWS_MATRIX.md. ---
echo.
echo [5/5] cl real munit suite (POSIX-clean subset, best-effort)
set MUNIT_PASS=0
set MUNIT_FAIL=0
set MUNIT_FAILED_LIST=
rem  space-separated MS:TN pairs (colon-delimited so no backslash parsing).
rem  test_tls is deliberately EXCLUDED: the MSVC build wires no TLS
rem  backend (XTC_TLS_BACKEND_* undefined in compat/xtc_config.h), so
rem  test_tls references TLS symbols not in xtc.lib and cannot link.
set MUNIT_TESTS=m0:test_version m0:test_errors m0:test_header m1:test_atomic m1:test_alloc m1:test_time m1:test_crypto m11:test_mctx m10:test_credit m10:test_fsm m10:test_pool m10:test_reg m10:test_stream m14:test_launch m14:test_unsafe_depth m14:test_stack_reclaim
for %%P in (%MUNIT_TESTS%) do (
  for /f "tokens=1,2 delims=:" %%a in ("%%P") do call :run_munit %%a %%b
)
echo.
echo [5/5] munit subset tally: !MUNIT_PASS! passed, !MUNIT_FAIL! failed
if not "!MUNIT_FAILED_LIST!"=="" (
  echo [5/5] munit FAILING:!MUNIT_FAILED_LIST!
  echo [5/5] MSVC munit subset gate FAILED
  goto :fail
)
echo [5/5] MSVC munit subset OK -- !MUNIT_PASS! POSIX-clean munit tests

rem  Explicit success so control never falls into :fail below.
exit /b 0

rem --- :run_munit <milestone> <testname>  (best-effort build+run of
rem     one munit test; bumps MUNIT_PASS / MUNIT_FAIL; always returns) ---
:run_munit
set MS=%~1
set TN=%~2
cl %CFLAGS% /Fe:%TN%.exe ^
   /I"%XTC_SRC%\test\%MS%" ^
   "%XTC_SRC%\test\%MS%\%TN%.c" ^
   "%XTC_SRC%\test\%MS%\munit.c" ^
   xtc.lib ws2_32.lib ntdll.lib dbghelp.lib >nul 2>&1
if errorlevel 1 (
  echo   [munit] %MS%\%TN% BUILD FAILED
  set /a MUNIT_FAIL+=1
  set MUNIT_FAILED_LIST=!MUNIT_FAILED_LIST! %MS%/%TN%
  goto :eof
)
%TN%.exe >nul 2>&1
if errorlevel 1 (
  echo   [munit] %MS%\%TN% TEST FAILED
  set /a MUNIT_FAIL+=1
  set MUNIT_FAILED_LIST=!MUNIT_FAILED_LIST! %MS%/%TN%
  goto :eof
)
echo   [munit] %MS%\%TN% OK
set /a MUNIT_PASS+=1
goto :eof

:fail
echo.
echo BUILD FAILED (errorlevel %errorlevel%)
exit /b 1
