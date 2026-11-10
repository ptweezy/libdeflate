@echo off
rem msasm.bat ARCH OUTDIR REF...  (test-only)
rem Writes OUTDIR\out\REF.asm, the cl /O2 /FAs listing of lib\deflate_compress.c.
setlocal enabledelayedexpansion
set ARCH=%1
set OUT=%~2
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VS=%%i
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% || exit /b 1
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
cl /nologo /O2 /W4 /WX /MD /c /I"%T%" /FAs /Fa"%OUT%\out\%REF%.asm" /Fo"%OUT%\%REF%.obj" "%T%\lib\deflate_compress.c" || exit /b 1
goto next
:done
echo ASM_OK %ARCH%
