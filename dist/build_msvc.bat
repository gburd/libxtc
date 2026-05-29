@echo off
rem ---------------------------------------------------------------------------
rem dist\build_msvc.bat
rem
rem   Build libxtc as a static library with the Microsoft toolchain
rem   (cl.exe + ml64.exe + lib.exe), and build one smoke test.
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
set CFLAGS=/nologo /std:c11 /experimental:c11atomics /W3 /O2 /MT /D_WIN32 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS %INC%

rem --- 1. Assemble the fcontext primitives with ml64. ---
echo [1/4] ml64 fctx_x86_64_ms_pe.asm
ml64 /nologo /c /Fo fctx_x86_64_ms_pe.obj "%XTC_SRC%\src\os\asm\fctx_x86_64_ms_pe.asm"
if errorlevel 1 goto :fail

rem --- 2. Compile the C sources.  Platform-specific backends
rem        (io_epoll/io_kqueue/io_solaris/io_uring/io_aix, coro_uctx)
rem        self-guard to empty translation units on Windows. ---
echo [2/4] cl compiling sources
set SRCS=
for %%D in (os io evt ptc orc) do (
  for %%F in ("%XTC_SRC%\src\%%D\*.c") do set SRCS=!SRCS! "%%F"
)
set SRCS=!SRCS! "%XTC_SRC%\src\xtc_version.c" "%XTC_SRC%\src\xtc_strerror.c"

cl %CFLAGS% /c !SRCS!
if errorlevel 1 goto :fail

rem --- 3. Archive into xtc.lib. ---
echo [3/4] lib xtc.lib
lib /nologo /OUT:xtc.lib *.obj
if errorlevel 1 goto :fail

rem --- 4. Build the MSVC smoke test (munit uses GCC-isms cl rejects;
rem        this standalone test exercises the Win32 paths directly). ---
echo [4/4] cl smoke.exe
cl %CFLAGS% /Fe:smoke.exe ^
   "%XTC_SRC%\test\msvc\smoke.c" ^
   xtc.lib ws2_32.lib
if errorlevel 1 goto :fail

echo.
echo BUILD OK: xtc.lib + smoke.exe
smoke.exe
goto :eof

:fail
echo.
echo BUILD FAILED (errorlevel %errorlevel%)
exit /b 1
