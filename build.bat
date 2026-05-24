@echo off
setlocal enabledelayedexpansion
set "ROOT=%~dp0"
cd /d "%ROOT%"

rem --- Locate Visual Studio via vswhere -------------------------------------
rem NOTE 1: VS 18 (2026) is on a prerelease channel, so we need -all -prerelease.
rem         -latest does NOT find it on this machine.
rem NOTE 2: we capture vswhere output via a temp file. A `for /f in (...)` over a
rem         quoted path that contains "(x86)" breaks cmd parsing, so we avoid it.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo [build] vswhere.exe not found & exit /b 1)
set "VSPATH="
"%VSWHERE%" -all -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\_wind_vspath.txt"
set /p VSPATH=<"%TEMP%\_wind_vspath.txt"
del "%TEMP%\_wind_vspath.txt" >nul 2>&1
if "%VSPATH%"=="" (echo [build] VC tools not found. Install "Desktop development with C++". & exit /b 1)
rem vcvars64.bat emits a harmless internal "vswhere not recognized" on this box;
rem suppress its output and rely on the errorlevel + the cl check below.
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo [build] vcvars64.bat failed & exit /b 1)
cd /d "%ROOT%"

if /i "%1"=="test" goto :test

rem --- App build ------------------------------------------------------------
cl /nologo /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE ^
   src\*.cpp ^
   /Fe:Wind.exe ^
   /link Magnification.lib Dwmapi.lib user32.lib shell32.lib gdi32.lib ^
   /MANIFEST:EMBED /MANIFESTINPUT:Wind.manifest /SUBSYSTEM:WINDOWS
exit /b %errorlevel%

rem --- Test build (pure-logic sources only; no <windows.h>) -----------------
:test
rem /wd5285 silences a known doctest 2.4.11 header warning under MSVC /W4.
cl /nologo /std:c++17 /EHsc /W4 /wd5285 /DWIND_TESTS /I third_party ^
   tests\*.cpp ^
   src\transform.cpp src\zoom_controller.cpp src\tracker.cpp src\config.cpp ^
   /Fe:wind_tests.exe
if errorlevel 1 exit /b 1
"%ROOT%wind_tests.exe"
exit /b %errorlevel%
