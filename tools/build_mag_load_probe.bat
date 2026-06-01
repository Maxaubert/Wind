@echo off
setlocal enabledelayedexpansion
set "ROOT=%~dp0.."
cd /d "%ROOT%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo [build] vswhere.exe not found & exit /b 1)
"%VSWHERE%" -all -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\_wind_vspath.txt"
set /p VSPATH=<"%TEMP%\_wind_vspath.txt"
del "%TEMP%\_wind_vspath.txt" >nul 2>&1
if "%VSPATH%"=="" (echo [build] VC tools not found & exit /b 1)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo [build] vcvars64 failed & exit /b 1)
cd /d "%ROOT%"

cl /nologo /std:c++17 /EHsc /W4 /DUNICODE /D_UNICODE ^
   tools\mag_load_probe.cpp ^
   /Fe:mag_load_probe.exe ^
   /link Magnification.lib d3d11.lib dxgi.lib d3dcompiler.lib user32.lib advapi32.lib /SUBSYSTEM:WINDOWS
exit /b %errorlevel%
