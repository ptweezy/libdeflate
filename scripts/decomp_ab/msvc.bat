@echo off
rem decomp_ab msvc.bat - test-only: build the bench harness for trees 0..N with
rem MSVC (x64).  usage: msvc.bat WORKDIR N
rem WORKDIR\tI must hold tree I; the result is WORKDIR\bI.exe.
setlocal
set W=%~1
set N=%~2
set VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq tokens=*" %%i in (`"%VSW%" -latest -products * -property installationPath`) do set VSI=%%i
if "%VSI%"=="" (echo Visual Studio not found & exit /b 1)
call "%VSI%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl 2>&1 | findstr /c:"Version"
for /l %%t in (0,1,%N%) do call :build %%t || exit /b 1
exit /b 0

:build
set T=%W%\t%1
if not exist "%W%\o%1" mkdir "%W%\o%1"
cl /nologo /std:c11 /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS /I"%T%" /Fo"%W%\o%1\\" /Fe"%W%\b%1.exe" "%~dp0bench.c" "%T%\lib\adler32.c" "%T%\lib\crc32.c" "%T%\lib\deflate_compress.c" "%T%\lib\deflate_decompress.c" "%T%\lib\gzip_compress.c" "%T%\lib\gzip_decompress.c" "%T%\lib\utils.c" "%T%\lib\zlib_compress.c" "%T%\lib\zlib_decompress.c" "%T%\lib\x86\cpu_features.c" > "%W%\build%1.log" 2>&1
if errorlevel 1 (type "%W%\build%1.log" & exit /b 1)
findstr /c:"warning" "%W%\build%1.log"
echo built tree %1
exit /b 0
