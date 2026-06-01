@echo off
setlocal EnableExtensions

cd /d "%~dp0"

set "MMX3_LOG=%~1"
if "%MMX3_LOG%"=="" set "MMX3_LOG=1"

if "%MMX3_LOG%"=="0" (
    set "BUILD_NAME=release"
) else (
    set "BUILD_NAME=debug"
)

echo === Mega Man X3 / Rockman X3 Portable Loader Build ===
echo Build type: %BUILD_NAME%
echo.

where cl >nul 2>nul
if %errorlevel%==0 (
    echo MSVC environment already available.
    goto build
)

echo MSVC environment not found. Trying to locate Visual Studio...

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found.
    echo Please install Visual Studio or Build Tools for Visual Studio.
    echo Required workload/component: Desktop development with C++
    pause
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSINSTALL=%%i"
)

if not defined VSINSTALL (
    echo ERROR: Visual Studio with C++ tools was not found.
    echo Please install "Desktop development with C++".
    pause
    exit /b 1
)

set "VSDEVCMD=%VSINSTALL%\Common7\Tools\VsDevCmd.bat"

if not exist "%VSDEVCMD%" (
    echo ERROR: VsDevCmd.bat not found:
    echo %VSDEVCMD%
    pause
    exit /b 1
)

echo Found Visual Studio:
echo %VSINSTALL%
echo.

call "%VSDEVCMD%" -arch=x86 -host_arch=x64

where cl >nul 2>nul
if not %errorlevel%==0 (
    echo ERROR: cl.exe still not available after VsDevCmd.
    pause
    exit /b 1
)

:build

echo.
echo Building ddraw.dll...
echo.

cl /nologo /LD /EHsc /O2 /DMMX3_ENABLE_LOG=%MMX3_LOG% ^
  ddraw_proxy.cpp ^
  mmx3_common.cpp ^
  mmx3_registry.cpp ^
  mmx3_cd.cpp ^
  mmx3_bugfix.cpp ^
  mmx3_timing.cpp ^
  user32.lib winmm.lib ^
  /link /DEF:ddraw_proxy.def /OUT:ddraw.dll

if not %errorlevel%==0 (
    echo.
    echo BUILD FAILED.
    pause
    exit /b 1
)

echo.
echo BUILD OK: ddraw.dll
echo.
echo Place ddraw.dll next to MMX3.exe or RMX3.exe.
pause
exit /b 0
