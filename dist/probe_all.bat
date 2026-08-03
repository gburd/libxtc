@echo off
rem Probe every TESTS_C test under MSVC: build, and if it builds, run.
rem Emits one line per test: PROBE <ms>/<tn> <BUILD_FAIL|RUN_FAIL|OK>
rem Assumes xtc.lib already built in %CD% (C:\xtcbuild).
setlocal enabledelayedexpansion
if "%XTC_SRC%"=="" set XTC_SRC=C:\libxtc
set INC=/I"%XTC_SRC%\src\inc" /I"%XTC_SRC%\src\inc\compat" /I"%XTC_SRC%\test\include" /I.
set CFLAGS=/nologo /std:c11 /experimental:c11atomics /W3 /O2 /MT /D_WIN32 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DXTC_BUILDING_DLL %INC%
set LIBS=xtc.lib ws2_32.lib ntdll.lib dbghelp.lib

set TESTS=%*
for %%P in (%TESTS%) do (
  for /f "tokens=1,2 delims=:" %%a in ("%%P") do call :probe %%a %%b
)
exit /b 0

:probe
set MS=%~1
set TN=%~2
copy /Y "%XTC_SRC%\test\m0\munit.h" "%XTC_SRC%\test\%MS%\munit.h" >nul 2>&1
copy /Y "%XTC_SRC%\test\m0\munit.c" "%XTC_SRC%\test\%MS%\munit.c" >nul 2>&1
del %TN%.exe >nul 2>&1
cl %CFLAGS% /Fe:%TN%.exe /I"%XTC_SRC%\test\%MS%" ^
   "%XTC_SRC%\test\%MS%\%TN%.c" "%XTC_SRC%\test\%MS%\munit.c" ^
   %LIBS% > build_%TN%.log 2>&1
if errorlevel 1 (
  echo PROBE %MS%/%TN% BUILD_FAIL
  goto :eof
)
%TN%.exe > run_%TN%.log 2>&1
if errorlevel 1 (
  echo PROBE %MS%/%TN% RUN_FAIL
  goto :eof
)
echo PROBE %MS%/%TN% OK
goto :eof
