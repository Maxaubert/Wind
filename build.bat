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
if /i "%1"=="check" goto :check
if /i "%1"=="uiaccess" goto :uiaccess
if /i "%1"=="config" goto :config
if /i "%1"=="installer" goto :installer

rem --- App build (normal: uiAccess=false, runs from anywhere) ----------------
rem Compile the app-icon resource (rc.exe ships with the Windows SDK, on PATH via vcvars).
rc /nologo /fo "%ROOT%src\wind.res" "%ROOT%src\wind.rc"
if errorlevel 1 (echo [build] rc.exe failed & exit /b 1)
cl /nologo /std:c++17 /EHsc /O2 /W4 /Zi /DUNICODE /D_UNICODE ^
   src\*.cpp src\config_ui\ini_edit.cpp src\wind.res ^
   /Fe:Wind.exe ^
   /link Magnification.lib Dwmapi.lib user32.lib shell32.lib gdi32.lib Dbghelp.lib ^
   d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib windowscodecs.lib ole32.lib advapi32.lib ^
   /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:Wind.manifest /SUBSYSTEM:WINDOWS ^
   /DEBUG /OPT:REF /OPT:ICF
exit /b %errorlevel%

rem --- UIAccess build (uiAccess=true: must be signed + run from Program Files) -
rem    Embeds Wind.uiaccess.manifest so the overlay CAN use a high z-band (opt-in zorderBand=16)
rem    to cover the Start menu / taskbar / tray. Shipped default is 0 - see issue #162.
rem    Deploy via tools\uiaccess_setup.ps1.
:uiaccess
rc /nologo /fo "%ROOT%src\wind.res" "%ROOT%src\wind.rc"
if errorlevel 1 (echo [build] rc.exe failed & exit /b 1)
cl /nologo /std:c++17 /EHsc /O2 /W4 /Zi /DUNICODE /D_UNICODE /DWIND_UIACCESS ^
   src\*.cpp src\config_ui\ini_edit.cpp src\wind.res ^
   /Fe:Wind.exe ^
   /link Magnification.lib Dwmapi.lib user32.lib shell32.lib gdi32.lib Dbghelp.lib ^
   d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib windowscodecs.lib ole32.lib advapi32.lib ^
   /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:Wind.uiaccess.manifest /SUBSYSTEM:WINDOWS ^
   /DEBUG /OPT:REF /OPT:ICF
exit /b %errorlevel%

rem --- Config UI host (WindConfig.exe). Builds the Svelte UI first if it exists. ----
:config
if exist "%ROOT%ui\package.json" (
  pushd "%ROOT%ui"
  if not exist node_modules ( call npm install || (popd & echo [build] npm install failed & exit /b 1) )
  call npm run build || (popd & echo [build] ui build failed & exit /b 1)
  popd
)
rem Same app-icon resource as Wind.exe (rc.exe ships with the Windows SDK, on PATH via vcvars).
rc /nologo /fo "%ROOT%src\wind.res" "%ROOT%src\wind.rc"
if errorlevel 1 (echo [build] rc.exe failed & exit /b 1)
cl /nologo /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE ^
   /I third_party\webview2\include ^
   src\config_ui\main.cpp src\config_ui\ini_edit.cpp src\profiles.cpp src\config.cpp src\logging.cpp src\wind.res ^
   /Fe:WindConfig.exe ^
   /link third_party\webview2\x64\WebView2LoaderStatic.lib ^
   user32.lib shell32.lib shlwapi.lib ole32.lib version.lib advapi32.lib ntdll.lib /SUBSYSTEM:WINDOWS
exit /b %errorlevel%

rem --- Test build (pure-logic sources only; no <windows.h>) -----------------
:test
rem /wd5285 silences a known doctest 2.4.11 header warning under MSVC /W4.
cl /nologo /std:c++17 /EHsc /W4 /wd5285 /DWIND_TESTS /I third_party ^
   tests\*.cpp ^
   src\transform.cpp src\zoom_controller.cpp src\config.cpp src\profiles.cpp src\cursor_mapper.cpp src\lock_detector.cpp src\cursor_lock.cpp src\mouse_ballistics.cpp src\crosshair.cpp src\config_ui\ini_edit.cpp src\logging.cpp ^
   /Fe:wind_tests.exe
if errorlevel 1 exit /b 1
"%ROOT%wind_tests.exe"
exit /b %errorlevel%

rem --- Compile-only check (no link; verifies all sources compile) -----------
:check
cl /nologo /std:c++17 /EHsc /W4 /DUNICODE /D_UNICODE /c src\*.cpp
exit /b %errorlevel%

rem --- Installer (needs NSIS; winget install NSIS.NSIS) ---------------------
:installer
set "MAKENSIS=%ProgramFiles(x86)%\NSIS\makensis.exe"
if not exist "%MAKENSIS%" set "MAKENSIS=%ProgramFiles%\NSIS\makensis.exe"
if not exist "%MAKENSIS%" (
  echo [build] NSIS not found. Install it with: winget install NSIS.NSIS
  exit /b 1
)
if not exist "%ROOT%dist" mkdir "%ROOT%dist"
rem /WX so a warning is a build failure. NSIS already aborts on a missing File source,
rem but it only WARNS about things like an unreferenced define or a shadowed function,
rem and in an installer those are how a page ends up wired to nothing.
"%MAKENSIS%" /WX /V2 "%ROOT%installer\wind.nsi"
if errorlevel 1 exit /b 1
rem The gate checks what a compile cannot: that every rectangle the pages read was
rem generated, and that a silent install/uninstall round-trips. Elevated-only parts
rem skip themselves from an ordinary shell.
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\installer_check.ps1"
exit /b %errorlevel%
