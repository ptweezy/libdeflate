@echo off
rem msbuildcm.bat ARCH OUTDIR NAME REF [NAME REF]...  (test-only)
rem Builds the static library of each REF with CMake (the configuration in
rem MSB_CONFIG, default Release; the project's own flags), then links
rem msbench.exe against it.  Also writes NAME.map and NAME.dis.
setlocal enabledelayedexpansion
set ARCH=%1
set OUT=%~2
set HERE=%~dp0
set CFG=Release
if not "%MSB_CONFIG%"=="" set CFG=%MSB_CONFIG%
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VS=%%i
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% || exit /b 1
set PLAT=x64
if "%ARCH%"=="arm64" set PLAT=ARM64
if "%ARCH%"=="x86" set PLAT=Win32
if not exist "%OUT%" mkdir "%OUT%"
shift
shift
:next
if "%1"=="" goto done
set NAME=%1
set REF=%2
shift
shift
set T=%OUT%\src-%NAME%
set B=%OUT%\b-%NAME%
if not exist "%T%" git worktree add --detach "%T%" %REF% || exit /b 1
cmake -S "%T%" -B "%B%" -A %PLAT% -DLIBDEFLATE_BUILD_GZIP=OFF -DLIBDEFLATE_BUILD_SHARED_LIB=OFF || exit /b 1
cmake --build "%B%" --config %CFG% -- /v:n > "%OUT%\%NAME%.buildlog" || exit /b 1
rem The library's runtime follows CMake's per-config flags (the project needs
rem only CMake 3.10, so CMP0091 is OLD): Release is the project's
rem "-O2 -DNDEBUG", which names no runtime, so cl's default /MT applies;
rem MinSizeRel is CMake's "/MD /O1 /Ob1 /DNDEBUG".  msbench must match, or
rem x86 fails to link (__except_handler4_common).
set RT=/MT
if /i "%CFG%"=="MinSizeRel" set RT=/MD
cl /nologo /O2 /W4 /WX %RT% /I"%T%" /Fo"%B%\\" /Fe"%OUT%\%NAME%.exe" /Fm"%OUT%\%NAME%.map" "%HERE%msbench.c" "%B%\%CFG%\deflatestatic.lib" || exit /b 1
dumpbin /nologo /disasm:nobytes "%OUT%\%NAME%.exe" > "%OUT%\%NAME%.dis" || exit /b 1
rem MSB_PADS (e.g. "16 32 48"): also link NAMEpP.exe with mspad.c's P-byte
rem function between msbench.obj and the library, which moves all of the
rem library's code by P bytes (mspadchk.py checks this).
if "%MSB_PADS%"=="" goto next
for %%P in (%MSB_PADS%) do (
	if not exist "%B%\pad%%P" mkdir "%B%\pad%%P"
	cl /nologo /O2 /W4 /WX %RT% /DPAD=%%P /I"%T%" /Fo"%B%\pad%%P\\" /Fe"%OUT%\%NAME%p%%P.exe" /Fm"%OUT%\%NAME%p%%P.map" "%HERE%msbench.c" "%HERE%mspad.c" "%B%\%CFG%\deflatestatic.lib" || exit /b 1
)
goto next
:done
echo BUILD_OK %ARCH% %CFG%
