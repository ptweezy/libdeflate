@echo off
rem msasm.bat ARCH OUTDIR REF...  (test-only)
rem For each REF: builds msbench.exe like msbuild.bat does and writes
rem OUTDIR\out\REF.map (link map) and OUTDIR\out\REF.dis (dumpbin /disasm).
setlocal enabledelayedexpansion
set ARCH=%1
set OUT=%~2
set HERE=%~dp0
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VS=%%i
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% || exit /b 1
set CPUF=lib\x86\cpu_features.c
if "%ARCH%"=="arm64" set CPUF=lib\arm\cpu_features.c
if not exist "%OUT%\out" mkdir "%OUT%\out"
cl 2> "%OUT%\out\cl-version.txt"
shift
shift
:next
if "%1"=="" goto done
set REF=%1
shift
set T=%OUT%\src-%REF%
if not exist "%T%" git worktree add --detach "%T%" %REF% || exit /b 1
if not exist "%OUT%\obj-%REF%" mkdir "%OUT%\obj-%REF%"
cl /nologo /O2 /W4 /WX /MD /I"%T%" /Fo"%OUT%\obj-%REF%\\" /Fe"%OUT%\%REF%.exe" /Fm"%OUT%\out\%REF%.map" "%HERE%msbench.c" "%T%\lib\adler32.c" "%T%\lib\crc32.c" "%T%\lib\deflate_compress.c" "%T%\lib\deflate_decompress.c" "%T%\lib\gzip_compress.c" "%T%\lib\gzip_decompress.c" "%T%\lib\utils.c" "%T%\lib\zlib_compress.c" "%T%\lib\zlib_decompress.c" "%T%\%CPUF%" || exit /b 1
dumpbin /nologo /disasm:nobytes "%OUT%\%REF%.exe" > "%OUT%\out\%REF%.dis" || exit /b 1
goto next
:done
echo ASM_OK %ARCH%
