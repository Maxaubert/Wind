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

rem --- App build (normal: uiAccess=false, runs from anywhere) ----------------
rem Compile the app-icon resource (rc.exe ships with the Windows SDK, on PATH via vcvars).
rc /nologo /fo "%ROOT%src\wind.res" "%ROOT%src\wind.rc"
if errorlevel 1 (echo [build] rc.exe failed & exit /b 1)
cl /nologo /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE ^
   src\*.cpp src\wind.res ^
   /Fe:Wind.exe ^
   /link Magnification.lib Dwmapi.lib user32.lib shell32.lib gdi32.lib ^
   d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib windowscodecs.lib ole32.lib ^
   /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:Wind.manifest /SUBSYSTEM:WINDOWS
exit /b %errorlevel%

rem --- UIAccess build (uiAccess=true: must be signed + run from Program Files) -
rem    Embeds Wind.uiaccess.manifest so the overlay can use a high z-band (zorderBand=16)
rem    to cover the Start menu / taskbar / tray. Deploy via tools\uiaccess_setup.ps1.
:uiaccess
rc /nologo /fo "%ROOT%src\wind.res" "%ROOT%src\wind.rc"
if errorlevel 1 (echo [build] rc.exe failed & exit /b 1)
cl /nologo /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE /DWIND_UIACCESS ^
   src\*.cpp src\wind.res ^
   /Fe:Wind.exe ^
   /link Magnification.lib Dwmapi.lib user32.lib shell32.lib gdi32.lib ^
   d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib windowscodecs.lib ole32.lib ^
   /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:Wind.uiaccess.manifest /SUBSYSTEM:WINDOWS
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
   src\config_ui\main.cpp src\config_ui\ini_edit.cpp src\logging.cpp src\wind.res ^
   /Fe:WindConfig.exe ^
   /link third_party\webview2\x64\WebView2LoaderStatic.lib ^
   user32.lib shell32.lib shlwapi.lib ole32.lib version.lib advapi32.lib ntdll.lib /SUBSYSTEM:WINDOWS
exit /b %errorlevel%

rem --- Test build (pure-logic sources only; no <windows.h>) -----------------
:test
rem /wd5285 silences a known doctest 2.4.11 header warning under MSVC /W4.
cl /nologo /std:c++17 /EHsc /W4 /wd5285 /DWIND_TESTS /I third_party ^
   tests\*.cpp ^
   src\transform.cpp src\zoom_controller.cpp src\config.cpp src\cursor_mapper.cpp src\lock_detector.cpp src\config_ui\ini_edit.cpp src\logging.cpp ^
   /Fe:wind_tests.exe
if errorlevel 1 exit /b 1
"%ROOT%wind_tests.exe"
exit /b %errorlevel%

rem --- Compile-only check (no link; verifies all sources compile) -----------
:check
cl /nologo /std:c++17 /EHsc /W4 /DUNICODE /D_UNICODE /c src\*.cpp
exit /b %errorlevel%

