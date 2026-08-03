@echo off
setlocal enabledelayedexpansion
if "%XTC_SRC%"=="" set XTC_SRC=C:\libxtc
set INC=/I"%XTC_SRC%\src\inc" /I"%XTC_SRC%\src\inc\compat" /I.
set CFLAGS=/nologo /std:c11 /experimental:c11atomics /W3 /WX /O2 /MT /D_WIN32 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DXTC_BUILDING_DLL %INC%
ml64 /nologo /c /Fo fctx_x86_64_ms_pe.obj "%XTC_SRC%\src\os\asm\fctx_x86_64_ms_pe.asm"
if errorlevel 1 exit /b 1
set SRCS=
for %%D in (os io evt ptc orc) do (
  for %%F in ("%XTC_SRC%\src\%%D\*.c") do (
    if /I not "%%~nxF"=="os_unsafe_stub.c" set SRCS=!SRCS! "%%F"
  )
)
set SRCS=!SRCS! "%XTC_SRC%\src\xtc_version.c" "%XTC_SRC%\src\xtc_strerror.c"
cl %CFLAGS% /c !SRCS!
if errorlevel 1 exit /b 1
lib /nologo /OUT:xtc.lib *.obj
if errorlevel 1 exit /b 1
echo LIBONLY_OK
exit /b 0
