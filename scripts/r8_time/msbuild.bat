@echo off
rem msbuild.bat ARCH OUTDIR NAME REF PAD [NAME REF PAD]...  (test-only)
rem Builds msbench.exe against the library of each REF with cl /O2 /W4 /WX.
rem r13pad.c is appended to lib/deflate_compress.c and R13_PAD=PAD is defined:
rem PAD 0 changes nothing, 16/32/48 adds an unused function of that size after
rem deflate_find_min_cost_path.  Also writes NAME.map and NAME.dis.
setlocal enabledelayedexpansion
set ARCH=%1
set OUT=%~2
set HERE=%~dp0
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VS=%%i
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% || exit /b 1
set CPUF=lib\x86\cpu_features.c
if "%ARCH%"=="arm64" set CPUF=lib\arm\cpu_features.c
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
if exist "%T%" goto havetree
git worktree add --detach "%T%" %REF% || exit /b 1
type "%HERE%r13pad.c" >> "%T%\lib\deflate_compress.c" || exit /b 1
:havetree
if not exist "%OUT%\obj-%NAME%" mkdir "%OUT%\obj-%NAME%"
cl /nologo /O2 /W4 /WX /MD /DR13_PAD=%PAD% /I"%T%" /Fo"%OUT%\obj-%NAME%\\" /Fe"%OUT%\%NAME%.exe" /Fm"%OUT%\%NAME%.map" "%HERE%msbench.c" "%T%\lib\adler32.c" "%T%\lib\crc32.c" "%T%\lib\deflate_compress.c" "%T%\lib\deflate_decompress.c" "%T%\lib\gzip_compress.c" "%T%\lib\gzip_decompress.c" "%T%\lib\utils.c" "%T%\lib\zlib_compress.c" "%T%\lib\zlib_decompress.c" "%T%\%CPUF%" || exit /b 1
dumpbin /nologo /disasm:nobytes "%OUT%\%NAME%.exe" > "%OUT%\%NAME%.dis" || exit /b 1
goto next
:done
echo BUILD_OK %ARCH%
