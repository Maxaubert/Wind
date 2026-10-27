@echo off
rem Copyright (c) 2026 Max Aubert. All rights reserved.
rem Proprietary and confidential source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
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

rem --- Build output ------------------------------------------------------------
rem Everything the compiler, linker and rc.exe produce goes under build\, never the repo root:
rem the binaries in build\ itself (the install layout, so a dev Wind.exe finds WindTray.exe,
rem WindConfig.exe and ui\dist next to it the way an installed one does), and objects, compiler
rem PDBs and .res files in build\obj\<target>\. One object dir per target, because targets compile
rem the same sources with different defines (WIND_UIACCESS, WIND_TESTS), and the tray and settings
rem hosts each have their own main.cpp. The working directory stays the repo root, so the test
rem binary finds tests\fixtures and a self-test writes its files where it is started from.
set "BUILD=%ROOT%build"
set "OBJ=%BUILD%\obj"
if not exist "%BUILD%" mkdir "%BUILD%"

rem --- Source folders ---------------------------------------------------------------
rem src\ is grouped by area (the source map in docs\architecture\01-overview.md). Wind headers are
rem included by bare file name ("config.h", never a path), so every compile gets every src folder on
rem its include path. A new folder goes in WIND_INC, and in CORE_SRC when it holds Wind.exe sources.
rem wind.rc, resource.h and version.h stay in src\ itself: the version readers expect them there.
set "WIND_INC=/I src /I src\core /I src\engine /I src\render /I src\transform /I src\color /I src\input /I src\cursor /I src\track /I src\zoom /I src\config /I src\tray /I src\platform /I src\tray_app /I src\config_ui"
set "CORE_SRC=src\core\*.cpp src\render\*.cpp src\transform\*.cpp src\color\*.cpp src\input\*.cpp src\cursor\*.cpp src\track\*.cpp src\zoom\*.cpp src\config\*.cpp src\tray\*.cpp"

if /i "%1"=="test" goto :test
if /i "%1"=="check" goto :check
if /i "%1"=="uiaccess" goto :uiaccess
if /i "%1"=="config" goto :config
if /i "%1"=="installer" goto :installer
if /i "%1"=="tray" goto :tray

rem --- App build (normal: uiAccess=false, runs from anywhere) ----------------
call :core_exe "" Wind.manifest wind
if errorlevel 1 exit /b 1
call :tray_exe
exit /b %errorlevel%

rem --- UIAccess build (uiAccess=true: must be signed + run from Program Files) -
rem    Embeds Wind.uiaccess.manifest so the overlay CAN use a high z-band (opt-in zorderBand=16)
rem    to cover the Start menu / taskbar / tray. Shipped default is 0 - see issue #162.
rem    Deploy via tools\uiaccess_setup.ps1.
:uiaccess
call :core_exe /DWIND_UIACCESS Wind.uiaccess.manifest wind_uiaccess
if errorlevel 1 exit /b 1
call :tray_exe
exit /b %errorlevel%

rem --- Wind.exe itself, shared by the app and uiaccess targets. %1 = extra define ("" for none),
rem    %2 = the manifest to embed, %3 = its object dir under build\obj. Everything else (sources,
rem    flags, libraries) is the same. Output: build\Wind.exe and build\Wind.pdb.
:core_exe
rem A new Wind.exe makes any build\WindUA.exe stale: the installer packs WindUA.exe as the
rem Wind.exe it installs whenever the file exists (installer\wind.nsi, LOCAL_SIGN), so a leftover
rem one would ship old code. tools\release.ps1 sets WIND_KEEP_UA for the one build that must keep
rem the WindUA.exe it has just made from the same tree.
if not "%WIND_KEEP_UA%"=="1" if exist "%BUILD%\WindUA.exe" del /q "%BUILD%\WindUA.exe"
set "CORE_OBJ=%OBJ%\%~3"
if not exist "%CORE_OBJ%" mkdir "%CORE_OBJ%"
rem Compile the app-icon resource (rc.exe ships with the Windows SDK, on PATH via vcvars).
rc /nologo /fo "%CORE_OBJ%\wind.res" "%ROOT%src\wind.rc"
if errorlevel 1 (echo [build] rc.exe failed & exit /b 1)
cl /nologo /std:c++17 /EHsc /O2 /W4 /Zi /DUNICODE /D_UNICODE %~1 %WIND_INC% ^
   /Fo"%CORE_OBJ%\\" /Fd"%CORE_OBJ%\\" ^
   %CORE_SRC% "%CORE_OBJ%\wind.res" ^
   /Fe:"%BUILD%\Wind.exe" ^
   /link Magnification.lib Dwmapi.lib user32.lib shell32.lib gdi32.lib Dbghelp.lib ^
   d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib windowscodecs.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib ^
   /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:%~2 /SUBSYSTEM:WINDOWS ^
   /DEBUG /OPT:REF /OPT:ICF
exit /b %errorlevel%

rem --- Tray helper (WindTray.exe, issue #291). ALWAYS the plain manifest (asInvoker, NO uiAccess):
rem    a UIAccess process's menu stacks above the cursor and the Snipping Tool overlay, which is the
rem    whole reason the tray lives in its own process. Built by the app and uiaccess targets too.
:tray
call :tray_exe
exit /b %errorlevel%

:tray_exe
if not exist "%OBJ%\tray" mkdir "%OBJ%\tray"
rc /nologo /fo "%OBJ%\tray\wind_tray.res" "%ROOT%src\tray_app\wind_tray.rc"
if errorlevel 1 (echo [build] rc.exe failed for WindTray & exit /b 1)
rem Own object dir (build\obj\tray) so the shared sources never overwrite Wind.exe's .obj files.
cl /nologo /std:c++17 /EHsc /O2 /W4 /Zi /DUNICODE /D_UNICODE %WIND_INC% ^
   /Fo"%OBJ%\tray\\" /Fd"%OBJ%\tray\\" ^
   src\tray_app\*.cpp src\tray\tray_items.cpp src\config\profiles.cpp src\config\config.cpp src\core\logging.cpp src\config\ini_edit.cpp ^
   "%OBJ%\tray\wind_tray.res" ^
   /Fe:"%BUILD%\WindTray.exe" ^
   /link user32.lib shell32.lib gdi32.lib Dwmapi.lib Dbghelp.lib shlwapi.lib ole32.lib version.lib ^
   advapi32.lib ntdll.lib ^
   /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:Wind.manifest /SUBSYSTEM:WINDOWS ^
   /DEBUG /OPT:REF /OPT:ICF
exit /b %errorlevel%

rem --- Config UI host (WindConfig.exe). Builds the Svelte UI first if it exists. ----
:config
if exist "%ROOT%ui\package.json" (
  pushd "%ROOT%ui"
  rem Explicit errorlevel checks: "call npm ... || (...)" does not reliably see npm.cmd's exit code,
  rem and a failed UI build once shipped a stale ui\dist (#347).
  if not exist node_modules call npm install
  if errorlevel 1 (popd & echo [build] npm install failed & exit /b 1)
  call npm run build
  if errorlevel 1 (popd & echo [build] ui build failed & exit /b 1)
  if not exist dist\index.html (popd & echo [build] ui build produced no dist\index.html & exit /b 1)
  popd
)
if not exist "%OBJ%\config" mkdir "%OBJ%\config"
rem Same app-icon resource as Wind.exe (rc.exe ships with the Windows SDK, on PATH via vcvars).
rc /nologo /fo "%OBJ%\config\wind.res" "%ROOT%src\wind.rc"
if errorlevel 1 (echo [build] rc.exe failed & exit /b 1)
cl /nologo /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE ^
   /I third_party\webview2\include %WIND_INC% ^
   /Fo"%OBJ%\config\\" /Fd"%OBJ%\config\\" ^
   src\config_ui\main.cpp src\config\ini_edit.cpp src\config\profiles.cpp src\config\config.cpp src\core\logging.cpp "%OBJ%\config\wind.res" ^
   /Fe:"%BUILD%\WindConfig.exe" ^
   /link third_party\webview2\x64\WebView2LoaderStatic.lib ^
   user32.lib shell32.lib shlwapi.lib ole32.lib version.lib advapi32.lib ntdll.lib Dwmapi.lib /SUBSYSTEM:WINDOWS
if errorlevel 1 exit /b 1
rem WindConfig.exe loads its page from <exe dir>\ui\dist, so mirror the UI into build\ui\dist, the
rem install layout. ui\dist stays the copy the installer and tools\uiaccess_setup.ps1 ship.
rem robocopy exit codes below 8 mean success.
if exist "%ROOT%ui\dist\index.html" (
  robocopy "%ROOT%ui\dist" "%BUILD%\ui\dist" /MIR /NJH /NJS /NFL /NDL /NP >nul
  if errorlevel 8 (echo [build] copying ui\dist to build\ui\dist failed & exit /b 1)
)
exit /b 0

rem --- Test build (pure-logic sources only; no <windows.h>) -----------------
:test
rem /wd5285 silences a known doctest 2.4.11 header warning under MSVC /W4.
if not exist "%OBJ%\test" mkdir "%OBJ%\test"
cl /nologo /std:c++17 /EHsc /W4 /wd5285 /DWIND_TESTS /I third_party %WIND_INC% ^
   /Fo"%OBJ%\test\\" /Fd"%OBJ%\test\\" ^
   tests\*.cpp ^
   src\transform\transform.cpp src\zoom\zoom_controller.cpp src\config\config.cpp src\config\profiles.cpp src\cursor\cursor_mapper.cpp src\cursor\lock_detector.cpp src\cursor\cursor_lock.cpp src\cursor\crosshair.cpp src\config\ini_edit.cpp src\core\logging.cpp src\tray\tray_items.cpp src\core\hitch_record.cpp src\render\render_params.cpp src\tray_app\tray_pin_logic.cpp ^
   /Fe:"%BUILD%\wind_tests.exe"
if errorlevel 1 exit /b 1
rem Runs from the repo root (the current directory): the tests find tests\fixtures from there.
"%BUILD%\wind_tests.exe"
exit /b %errorlevel%

rem --- Compile-only check (no link; verifies all sources compile) -----------
:check
for %%D in (core tray config) do if not exist "%OBJ%\check\%%D" mkdir "%OBJ%\check\%%D"
cl /nologo /std:c++17 /EHsc /W4 /DUNICODE /D_UNICODE /c %WIND_INC% /Fo"%OBJ%\check\core\\" %CORE_SRC%
if errorlevel 1 exit /b 1
rem WindTray.exe sources (issue #291); own object dir, its main.cpp would overwrite Wind's main.obj.
cl /nologo /std:c++17 /EHsc /W4 /DUNICODE /D_UNICODE /c %WIND_INC% /Fo"%OBJ%\check\tray\\" src\tray_app\*.cpp
if errorlevel 1 exit /b 1
rem WindConfig.exe's host (same flags as :config); own object dir for the same reason.
cl /nologo /std:c++17 /EHsc /W4 /DUNICODE /D_UNICODE /c /I third_party\webview2\include %WIND_INC% ^
   /Fo"%OBJ%\check\config\\" src\config_ui\main.cpp
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
