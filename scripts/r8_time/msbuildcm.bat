@echo off
rem msbuildcm.bat ARCH OUTDIR NAME REF PAD [NAME REF PAD]...  (test-only)
rem Builds the static library of each REF with CMake (Release configuration,
rem the project's own flags), then links msbench.exe against it.  Also writes
rem NAME.map and NAME.dis.  r13pad.c is appended to lib/deflate_compress.c
rem and R13_PAD=PAD is defined, as in msbuild.bat; PAD 0 adds no code.
setlocal enabledelayedexpansion
set ARCH=%1
set OUT=%~2
set HERE=%~dp0
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
set PAD=%3
shift
shift
shift
set T=%OUT%\src-%NAME%
set B=%OUT%\b-%NAME%
if exist "%T%" goto havetree
git worktree add --detach "%T%" %REF% || exit /b 1
type "%HERE%r13pad.c" >> "%T%\lib\deflate_compress.c" || exit /b 1
:havetree
cmake -S "%T%" -B "%B%" -A %PLAT% -DLIBDEFLATE_BUILD_GZIP=OFF -DLIBDEFLATE_BUILD_SHARED_LIB=OFF "-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /DR13_PAD=%PAD%" || exit /b 1
cmake --build "%B%" --config Release -- /v:n > "%OUT%\%NAME%.buildlog" || exit /b 1
rem CMake's Release flags for the library are /O2 /DNDEBUG /std:c11 with the
rem static runtime, so msbench uses /MT too.
cl /nologo /O2 /W4 /WX /MT /I"%T%" /Fo"%B%\\" /Fe"%OUT%\%NAME%.exe" /Fm"%OUT%\%NAME%.map" "%HERE%msbench.c" "%B%\Release\deflatestatic.lib" || exit /b 1
dumpbin /nologo /disasm:nobytes "%OUT%\%NAME%.exe" > "%OUT%\%NAME%.dis" || exit /b 1
goto next
:done
echo BUILD_OK %ARCH%
