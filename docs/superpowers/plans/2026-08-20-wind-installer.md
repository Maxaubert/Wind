# Wind Installer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship `Wind-Setup-x64-<version>.exe` for GitHub Releases: a per-machine elevated installer that puts Wind in `C:\Program Files\Wind`, presents itself as a custom-drawn video screen like Prism's setup, and uninstalls cleanly.

**Architecture:** NSIS 3.x driven by a hand-written script. Functionality lands first behind stock NSIS UI and is verified end to end; the custom presentation layer (ported from Prism's `kit.nsh` / `video.nsh`, with an overlay renderer swapped from Electron to Playwright) is then layered on top and replaces the stock pages. Pure decision logic (version comparison, WebView2 probe) lives in headers under `src/` and is doctested with the rest of the suite.

**Tech Stack:** NSIS 3.x (`winget install NSIS.NSIS`), PowerShell 7 for the release script, Node + Playwright (already in `ui/package.json`) for overlay rendering, ImageMagick and ffmpeg for the frame pipeline, MSVC/doctest for the pure-logic tests.

**Spec:** `docs/superpowers/specs/2026-08-20-installer-design.md`

## Global Constraints

- **No em-dashes (U+2014) anywhere** - code, comments, docs, commit messages, installer copy. Use en-dashes, commas, or rephrase. No `&mdash;` in `over.html`.
- **Pure-logic files must not include `<windows.h>`.** `src/installer_state.h` and `src/webview2_probe.h` are pure headers; the test build compiles `tests\*.cpp` with `/DWIND_TESTS` and no Windows headers.
- **Install root is `C:\Program Files\Wind`, fixed and not user-editable.** UIAccess requires a secure location; a browsable path would silently disable it.
- **Never hardcode `L"magnifier.ini"`.** The app owns its ini via `wind::ResolveIniPath()`; the installer writes no ini at all.
- **The installer writes nothing to `%LOCALAPPDATA%\Wind`.** The app seeds and owns that directory.
- **Autostart goes in `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run`, never HKCU.** An elevated installer's HKCU is the wrong hive.
- **Wind is launched de-elevated** via `ShellExecAsUser`, never plain `Exec`.
- **Quit a running Wind by setting the named event `Local\Wind_QuitRequest`**, waiting up to 5000 ms, and only then killing. Clean exit restores the OS cursor, releases `ClipCursor`, and restores the user's native-Magnifier registry backup.
- **Version comes from `src/version.h`** (`WIND_VER_MAJOR` / `WIND_VER_MINOR` / `WIND_VER_PATCH`) via `!searchparse`. Nothing else declares a version.
- **Signing is environment-driven** (`WIND_SIGN_THUMBPRINT`, or `WIND_SIGN_PFX` + `WIND_SIGN_PASSWORD`). No certificate, thumbprint or password enters the repository.
- **Overlay rectangles in `over.nsh` are in 640x480 units.** `kit.nsh` scales them by `$Dpi / 96`.
- **`installer/media/` is generated.** Never hand-edit it; regenerate with `make-over.mjs` / `make-loop.mjs`.
- **Feature work is issue -> branch -> PR** against `github.com/Maxaubert/Wind`.

---

### Task 1: Pure decision logic for install state and WebView2

**Files:**
- Create: `src/installer_state.h`
- Create: `src/webview2_probe.h`
- Test: `tests/test_installer_state.cpp`
- Test: `tests/test_webview2_probe.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum class wind::InstallState { Fresh, Upgrade, Reinstall, Downgrade };`
  - `wind::Version wind::ParseVersion(const std::string& s);` where `struct Version { int major=0, minor=0, patch=0; bool valid=false; };`
  - `int wind::CompareVersion(const Version& a, const Version& b);` returning -1, 0 or 1.
  - `wind::InstallState wind::ClassifyInstall(const std::string& found, const std::string& ours);`
  - `bool wind::WebView2Present(const std::string& pv);`

These are consumed by `installer/app.nsh` (Task 3) as documented behaviour, not as linked code: NSIS reimplements the same rules in six lines, and these tests are what pin the rules down.

- [ ] **Step 1: Write the failing tests**

`tests/test_installer_state.cpp`:

```cpp
#include "doctest.h"
#include "../src/installer_state.h"

using namespace wind;

TEST_CASE("ParseVersion reads a three-part version") {
    Version v = ParseVersion("1.2.3");
    CHECK(v.valid);
    CHECK(v.major == 1);
    CHECK(v.minor == 2);
    CHECK(v.patch == 3);
}

TEST_CASE("ParseVersion tolerates a four-part version and ignores the build field") {
    Version v = ParseVersion("0.1.0.0");
    CHECK(v.valid);
    CHECK(v.major == 0);
    CHECK(v.minor == 1);
    CHECK(v.patch == 0);
}

TEST_CASE("ParseVersion rejects junk") {
    CHECK_FALSE(ParseVersion("").valid);
    CHECK_FALSE(ParseVersion("not-a-version").valid);
    CHECK_FALSE(ParseVersion("1.2").valid);
}

TEST_CASE("CompareVersion orders by major then minor then patch") {
    CHECK(CompareVersion(ParseVersion("1.0.0"), ParseVersion("0.9.9")) == 1);
    CHECK(CompareVersion(ParseVersion("0.1.0"), ParseVersion("0.1.0")) == 0);
    CHECK(CompareVersion(ParseVersion("0.1.2"), ParseVersion("0.1.10")) == -1);
}

TEST_CASE("ClassifyInstall calls an absent previous version a fresh install") {
    CHECK(ClassifyInstall("", "0.2.0") == InstallState::Fresh);
    CHECK(ClassifyInstall("garbage", "0.2.0") == InstallState::Fresh);
}

TEST_CASE("ClassifyInstall separates upgrade, reinstall and downgrade") {
    CHECK(ClassifyInstall("0.1.0", "0.2.0") == InstallState::Upgrade);
    CHECK(ClassifyInstall("0.2.0", "0.2.0") == InstallState::Reinstall);
    CHECK(ClassifyInstall("0.3.0", "0.2.0") == InstallState::Downgrade);
}
```

`tests/test_webview2_probe.cpp`:

```cpp
#include "doctest.h"
#include "../src/webview2_probe.h"

using namespace wind;

TEST_CASE("WebView2Present accepts a real Evergreen version string") {
    CHECK(WebView2Present("120.0.2210.91"));
    CHECK(WebView2Present("109.0.1518.78"));
}

TEST_CASE("WebView2Present treats absent, empty and the zero sentinel as missing") {
    CHECK_FALSE(WebView2Present(""));
    CHECK_FALSE(WebView2Present("0.0.0.0"));
    CHECK_FALSE(WebView2Present("0.0.0"));
}

TEST_CASE("WebView2Present rejects a value that is not a version at all") {
    CHECK_FALSE(WebView2Present("unknown"));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `build.bat test`
Expected: FAIL at compile with `cannot open include file: '../src/installer_state.h'`.

- [ ] **Step 3: Write the implementations**

`src/installer_state.h`:

```cpp
#pragma once
// Pure decision logic shared between the installer's NSIS script and the test suite.
// NSIS reimplements these rules in its own dialect; these definitions are what the rules
// are checked against, so a change here is a change the installer must follow.
// NO <windows.h>: this header is compiled into the desktop-free test binary.
#include <string>

namespace wind {

struct Version {
    int  major = 0;
    int  minor = 0;
    int  patch = 0;
    bool valid = false;
};

enum class InstallState { Fresh, Upgrade, Reinstall, Downgrade };

// "1.2.3" or "1.2.3.4" (the build field is read and discarded: ARP writes three parts,
// VERSIONINFO writes four, and they must compare equal). Anything else is invalid.
inline Version ParseVersion(const std::string& s) {
    Version v;
    int part[4] = {0, 0, 0, 0};
    int n = 0;                 // parts filled
    bool digits = false;       // saw at least one digit in the current part
    for (size_t i = 0; i <= s.size(); ++i) {
        const char c = (i < s.size()) ? s[i] : '.';
        if (c >= '0' && c <= '9') {
            if (n >= 4) return Version{};          // more parts than a version has
            part[n] = part[n] * 10 + (c - '0');
            digits = true;
        } else if (c == '.') {
            if (!digits) return Version{};         // ".." or a leading/trailing dot
            ++n;
            digits = false;
            if (i == s.size()) break;
        } else {
            return Version{};                      // any other character
        }
    }
    if (n < 3) return Version{};                   // "1.2" is not a version here
    v.major = part[0];
    v.minor = part[1];
    v.patch = part[2];
    v.valid = true;
    return v;
}

inline int CompareVersion(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

// `found` is whatever DisplayVersion the ARP key held, which is "" on a clean machine and
// can be junk left by a half-removed install. Either way there is nothing to upgrade from.
inline InstallState ClassifyInstall(const std::string& found, const std::string& ours) {
    const Version f = ParseVersion(found);
    const Version o = ParseVersion(ours);
    if (!f.valid || !o.valid) return InstallState::Fresh;
    const int c = CompareVersion(f, o);
    if (c < 0) return InstallState::Upgrade;
    if (c == 0) return InstallState::Reinstall;
    return InstallState::Downgrade;
}

}  // namespace wind
```

`src/webview2_probe.h`:

```cpp
#pragma once
// Is the WebView2 Evergreen runtime installed? The answer is a registry string, and the
// only subtlety is that Microsoft's uninstaller leaves the value behind set to "0.0.0.0"
// rather than deleting it, so a non-empty value is not proof of presence.
// WindConfig.exe paints an empty shell without the runtime, which is why this is checked.
// NO <windows.h>: pure, so the rule is testable.
#include <string>
#include "installer_state.h"

namespace wind {

inline bool WebView2Present(const std::string& pv) {
    const Version v = ParseVersion(pv);
    if (!v.valid) return false;
    return !(v.major == 0 && v.minor == 0 && v.patch == 0);
}

}  // namespace wind
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `build.bat test`
Expected: PASS, and the summary's assertion count is higher than before this task.

- [ ] **Step 5: Commit**

```bash
git add src/installer_state.h src/webview2_probe.h tests/test_installer_state.cpp tests/test_webview2_probe.cpp
git commit -m "feat(installer): pure install-state and WebView2 probe logic"
```

---

### Task 2: A working installer behind stock NSIS UI

Functionality first. At the end of this task there is a real, ugly, working `Wind-Setup-x64-0.1.0.exe`. The picture goes on in Tasks 6 to 9.

**Files:**
- Create: `LICENSE` (MIT, 2026, Max Aubert)
- Create: `installer/wind.nsi`
- Modify: `build.bat` (add the `installer` target and NSIS discovery)
- Modify: `.gitignore` (ignore `installer/media/`, `dist/`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `installer/wind.nsi` defining `${WIND_VERSION}`, `${WIND_VER_MAJOR/MINOR/PATCH}`, `${INSTALL_DIR}`, `${ARP_KEY}`, `${RUN_KEY}`, `${RUN_VALUE}` for Tasks 3 and 9.
  - `dist/Wind-Setup-x64-<version>.exe`.
  - `build.bat installer`.

- [ ] **Step 1: Install NSIS and confirm it is on PATH**

Run: `winget install --id NSIS.NSIS --silent --accept-package-agreements --accept-source-agreements`
Then: `& "C:\Program Files (x86)\NSIS\makensis.exe" /VERSION`
Expected: a version string of `v3.` or higher. NSIS does not add itself to PATH, which is why `build.bat` discovers it by path in Step 4.

- [ ] **Step 2: Add the LICENCE**

SignPath Foundation will not issue a free certificate to a repository without an OSS licence, and Wind has none. Create `LICENSE` with the standard MIT text, `Copyright (c) 2026 Max Aubert`.

- [ ] **Step 3: Write `installer/wind.nsi`**

```nsis
;
; Wind setup.
;
; Per-machine, elevated, into Program Files, because UIAccess is only granted to a signed
; binary in a secure location. The path is therefore not the user's to change: installing
; to D:\Apps\Wind would silently disable the features this install mode exists to enable.
;
; This file is the whole installer while the picture is being built. installer\pages.nsh
; replaces the stock pages later; nothing in the sections changes when it does.
;

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma

; ---- version: src\version.h is the only place a version is declared ----------
!searchparse /file "..\src\version.h" "#define WIND_VER_MAJOR " WIND_VER_MAJOR
!searchparse /file "..\src\version.h" "#define WIND_VER_MINOR " WIND_VER_MINOR
!searchparse /file "..\src\version.h" "#define WIND_VER_PATCH " WIND_VER_PATCH
!define WIND_VERSION "${WIND_VER_MAJOR}.${WIND_VER_MINOR}.${WIND_VER_PATCH}"

!define PRODUCT      "Wind"
!define PUBLISHER    "Max Aubert"
!define INSTALL_DIR  "$PROGRAMFILES64\Wind"
!define ARP_KEY      "Software\Microsoft\Windows\CurrentVersion\Uninstall\Wind"
!define RUN_KEY      "Software\Microsoft\Windows\CurrentVersion\Run"
!define RUN_VALUE    "Wind"

Name "${PRODUCT}"
OutFile "..\dist\Wind-Setup-x64-${WIND_VERSION}.exe"
InstallDir "${INSTALL_DIR}"
RequestExecutionLevel admin
ShowInstDetails hide
ShowUninstDetails hide

VIProductVersion "${WIND_VER_MAJOR}.${WIND_VER_MINOR}.${WIND_VER_PATCH}.0"
VIAddVersionKey "ProductName"     "${PRODUCT}"
VIAddVersionKey "FileDescription" "Wind setup"
VIAddVersionKey "FileVersion"     "${WIND_VERSION}"
VIAddVersionKey "ProductVersion"  "${WIND_VERSION}"
VIAddVersionKey "CompanyName"     "${PUBLISHER}"
VIAddVersionKey "LegalCopyright"  "Copyright (c) 2026 ${PUBLISHER}"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!define MUI_ICON   "..\assets\wind.ico"
!define MUI_UNICON "..\assets\wind.ico"

; Stock pages for now. pages.nsh replaces this block in Task 9.
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; Setup writes nothing to %LOCALAPPDATA%\Wind: the app seeds and owns that directory,
; including magnifier.ini, which it resolves through wind::ResolveIniPath().
Section "Wind" SEC_WIND
  SetOutPath "$INSTDIR"
  File "..\Wind.exe"
  File "..\WindConfig.exe"
  SetOutPath "$INSTDIR\ui\dist"
  File /r "..\ui\dist\*.*"
  SetOutPath "$INSTDIR"

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Add or remove programs
  WriteRegStr   HKLM "${ARP_KEY}" "DisplayName"     "${PRODUCT}"
  WriteRegStr   HKLM "${ARP_KEY}" "DisplayVersion"  "${WIND_VERSION}"
  WriteRegStr   HKLM "${ARP_KEY}" "Publisher"       "${PUBLISHER}"
  WriteRegStr   HKLM "${ARP_KEY}" "DisplayIcon"     "$INSTDIR\Wind.exe"
  WriteRegStr   HKLM "${ARP_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKLM "${ARP_KEY}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegDWORD HKLM "${ARP_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${ARP_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${ARP_KEY}" "EstimatedSize" "$0"

  ; Start menu. SetShellVarContext all, because this is a per-machine install.
  SetShellVarContext all
  CreateShortcut "$SMPROGRAMS\Wind.lnk" "$INSTDIR\Wind.exe"
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  Delete "$SMPROGRAMS\Wind.lnk"
  Delete "$DESKTOP\Wind.lnk"
  DeleteRegValue HKLM "${RUN_KEY}" "${RUN_VALUE}"
  DeleteRegKey   HKLM "${ARP_KEY}"

  Delete "$INSTDIR\Wind.exe"
  Delete "$INSTDIR\WindConfig.exe"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir /r "$INSTDIR\ui"
  ; Not RMDir /r on $INSTDIR itself: it is a user-chosen-looking path and a stray
  ; recursive delete of the wrong directory is the one unrecoverable installer bug.
  RMDir "$INSTDIR"
SectionEnd

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Wind requires 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd
```

- [ ] **Step 4: Add the `installer` target to `build.bat`**

Append a target after `:check`, and add `installer` to the dispatch at the top of the file alongside `test`, `uiaccess` and `config`:

```bat
rem --- Installer (needs NSIS; winget install NSIS.NSIS) ---------------------
:installer
set "MAKENSIS=%ProgramFiles(x86)%\NSIS\makensis.exe"
if not exist "%MAKENSIS%" set "MAKENSIS=%ProgramFiles%\NSIS\makensis.exe"
if not exist "%MAKENSIS%" (
  echo [build] NSIS not found. Install it with: winget install NSIS.NSIS
  exit /b 1
)
if not exist "%ROOT%dist" mkdir "%ROOT%dist"
rem /WX so a warning is a build failure: NSIS warns rather than errors on a missing
rem File source, which would otherwise ship an installer with nothing in it.
"%MAKENSIS%" /WX /V2 "%ROOT%installer\wind.nsi"
exit /b %errorlevel%
```

- [ ] **Step 5: Ignore the generated output**

Add to `.gitignore`:

```
dist/
installer/media/
```

- [ ] **Step 6: Build the payload and the installer**

Run: `build.bat` then `build.bat config` then `build.bat installer`
Expected: `dist\Wind-Setup-x64-0.1.0.exe` exists and `makensis` reported no warnings.

- [ ] **Step 7: Verify install and uninstall by hand**

Run the installer, accept the UAC prompt, click through the stock pages. Then check:

```powershell
Test-Path 'C:\Program Files\Wind\Wind.exe'
Test-Path 'C:\Program Files\Wind\ui\dist\index.html'
Get-ItemProperty 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Wind' |
  Select-Object DisplayName, DisplayVersion, Publisher, EstimatedSize
```

Expected: all present, `DisplayVersion` is `0.1.0`, and Wind appears in Settings > Apps.
Then uninstall from Settings and re-run the checks: everything gone.

- [ ] **Step 8: Commit**

```bash
git add LICENSE installer/wind.nsi build.bat .gitignore
git commit -m "feat(installer): per-machine NSIS installer, ARP entry and uninstaller"
```

---

### Task 3: Wind-specific install behaviour

**Files:**
- Create: `installer/app.nsh`
- Modify: `installer/wind.nsi` (include `app.nsh`, call its macros from the sections)

**Interfaces:**
- Consumes: `${ARP_KEY}`, `${RUN_KEY}`, `${RUN_VALUE}`, `${WIND_VERSION}` from `installer/wind.nsi`.
- Produces, for `installer/pages.nsh` (Task 9):
  - `Var WantAutostart` - 1 by default, read by `WIND_APPLY_AUTOSTART`
  - `Var WantDesktop` - 0 by default
  - `Var RunAfter` - 1 by default
  - `!insertmacro WIND_QUIT_RUNNING` - stop a live Wind and WindConfig
  - `!insertmacro WIND_ENSURE_WEBVIEW2` - bootstrap the runtime if absent
  - `!insertmacro WIND_APPLY_AUTOSTART` - write or remove the HKLM Run value
  - `!insertmacro WIND_LAUNCH_DEELEVATED` - start Wind as the signed-in user

- [ ] **Step 1: Add the plugin dependency**

`ShellExecAsUser` ships with NSIS 3 as `Plugins\x86-unicode\ShellExecAsUser.dll`. Confirm it:

Run: `Test-Path "C:\Program Files (x86)\NSIS\Plugins\x86-unicode\ShellExecAsUser.dll"`
Expected: `True`. If `False`, download the plugin from the NSIS wiki into that folder; the script cannot compile without it.

- [ ] **Step 2: Write `installer/app.nsh`**

```nsis
;
; Wind setup, the parts that are about Wind rather than about installing.
;
; Four things the generic script does not know: how to make a running Wind let go of its
; own exe, whether WindConfig has a browser engine to run in, where autostart lives when
; the installer is elevated, and how to hand the app back to the user who asked for it.
;

!include "LogicLib.nsh"
!include "FileFunc.nsh"

!define WV2_CLIENT "SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"

Var WantAutostart
Var WantDesktop
Var RunAfter

; ---- stop a running Wind -----------------------------------------------------
; An upgrade always runs over a live tray app holding its own exe open. Wind exposes an
; auto-reset named event for exactly this (src\main.cpp:2199); setting it makes Wind exit
; CLEANLY, which restores the OS cursor, releases any ClipCursor, releases the shared
; Magnification runtime, and restores the user's native-Magnifier registry backup if the
; magnify model ever modified it. Killing the process skips all of that and can leave the
; pointer hidden or pinned to one pixel. So: ask, wait, and only then kill.
;
; Local\ is session-scoped and UAC elevation stays inside the session, so the elevated
; installer and the user's Wind see the same object.
!macro WIND_QUIT_RUNNING
  DetailPrint "Closing Wind..."
  ; WindConfig first: it holds no OS state, and closing it stops it relaunching Wind.
  nsExec::Exec 'taskkill /IM WindConfig.exe /F'
  Pop $0

  ; EVENT_MODIFY_STATE = 0x0002
  System::Call 'kernel32::OpenEventW(i 0x0002, i 0, w "Local\Wind_QuitRequest") p .r0'
  ${If} $0 <> 0
    System::Call 'kernel32::SetEvent(p $0)'
    System::Call 'kernel32::CloseHandle(p $0)'
    ; Up to 5 s in 250 ms steps, so a quick exit costs a quarter second, not five.
    StrCpy $1 0
    ${Do}
      Sleep 250
      IntOp $1 $1 + 1
      nsExec::Exec 'cmd /c tasklist /FI "IMAGENAME eq Wind.exe" /NH | find /I "Wind.exe"'
      Pop $2
      ${If} $2 != 0
        ${Break}          ; find returned non-zero: no such process, it is gone
      ${EndIf}
    ${LoopUntil} $1 >= 20
  ${EndIf}

  ; Whatever is left after the polite request gets terminated. The installer is elevated,
  ; so this succeeds even against the signed UIAccess build's integrity level.
  nsExec::Exec 'taskkill /IM Wind.exe /F'
  Pop $0
  Sleep 300
!macroend

; ---- WebView2 ----------------------------------------------------------------
; WindConfig.exe paints an empty shell without the Evergreen runtime. Microsoft's
; uninstaller leaves `pv` behind set to "0.0.0.0" rather than deleting it, so a value
; being present is not proof (src\webview2_probe.h pins that rule and tests it).
!macro WIND_ENSURE_WEBVIEW2
  StrCpy $0 ""
  ReadRegStr $0 HKLM "${WV2_CLIENT}" "pv"
  ${If} $0 == ""
    ReadRegStr $0 HKCU "${WV2_CLIENT}" "pv"
  ${EndIf}
  ${If} $0 == ""
  ${OrIf} $0 == "0.0.0.0"
    DetailPrint "Installing the WebView2 runtime..."
    ; ~2 MB stub that pulls the runtime itself, rather than bundling ~150 MB we would
    ; ship to every user to serve the small minority that lack it.
    File "/oname=$PLUGINSDIR\MicrosoftEdgeWebview2Setup.exe" "MicrosoftEdgeWebview2Setup.exe"
    nsExec::Exec '"$PLUGINSDIR\MicrosoftEdgeWebview2Setup.exe" /silent /install'
    Pop $0
    ${If} $0 != 0
      ; Not fatal. Wind.exe itself needs no browser engine; only Settings does, and it
      ; can be repaired later. Failing the whole install over it would be worse.
      DetailPrint "WebView2 install returned $0; Settings may not open until it is installed."
    ${EndIf}
  ${EndIf}
!macroend

; ---- autostart ---------------------------------------------------------------
; HKLM, not HKCU. This installer is elevated, and an HKCU write from an elevated process
; lands in whichever hive the elevated token owns, which is the administrator's rather
; than the user's whenever a standard user elevated with a different account. HKLM Run is
; also what a per-machine install should use, and Task Manager's Startup tab lists it.
!macro WIND_APPLY_AUTOSTART
  ${If} $WantAutostart == 1
    WriteRegStr HKLM "${RUN_KEY}" "${RUN_VALUE}" '"$INSTDIR\Wind.exe"'
  ${Else}
    DeleteRegValue HKLM "${RUN_KEY}" "${RUN_VALUE}"
  ${EndIf}
!macroend

; ---- launching the app -------------------------------------------------------
; Plain Exec would hand Wind the installer's admin token. wind::ResolveIniPath() would
; then resolve %LOCALAPPDATA% to the ADMINISTRATOR's profile, so magnifier.ini, the
; profiles and the logs would land where the user never finds them, and a tray magnifier
; would run elevated forever for no reason. ShellExecAsUser re-parents the launch to the
; shell, which gives it the signed-in user's token.
!macro WIND_LAUNCH_DEELEVATED
  ShellExecAsUser::ShellExecAsUser "open" "$INSTDIR\Wind.exe" "" ""
!macroend
```

- [ ] **Step 3: Fetch the WebView2 bootstrapper into the installer folder**

```powershell
Invoke-WebRequest -Uri 'https://go.microsoft.com/fwlink/p/?LinkId=2124703' `
  -OutFile 'installer\MicrosoftEdgeWebview2Setup.exe'
(Get-Item 'installer\MicrosoftEdgeWebview2Setup.exe').Length
```

Expected: roughly 1.5 to 2.5 MB. This file IS committed (it is a redistributable stub, not generated output), so add an exception in `.gitignore` if a pattern would catch it.

- [ ] **Step 4: Wire the macros into `installer/wind.nsi`**

Add after the other includes:

```nsis
!include "app.nsh"
```

Inside `Section "Wind"`, before `SetOutPath "$INSTDIR"`:

```nsis
  !insertmacro WIND_QUIT_RUNNING
```

Inside `Section "Wind"`, after the shortcut block:

```nsis
  !insertmacro WIND_ENSURE_WEBVIEW2
  !insertmacro WIND_APPLY_AUTOSTART
  ${If} $WantDesktop == 1
    CreateShortcut "$DESKTOP\Wind.lnk" "$INSTDIR\Wind.exe"
  ${EndIf}
```

Inside `Section "Uninstall"`, as the first line after `SetShellVarContext all`:

```nsis
  !insertmacro WIND_QUIT_RUNNING
```

In `.onInit`, after `SetRegView 64`, seed the defaults the pages will later edit:

```nsis
  StrCpy $WantAutostart 1
  StrCpy $WantDesktop 0
  StrCpy $RunAfter 1
```

And give the stock finish page something to do until Task 9 replaces it, before the `MUI_PAGE_FINISH` line:

```nsis
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION WindLaunch
```

with the function defined after the language block:

```nsis
Function WindLaunch
  !insertmacro WIND_LAUNCH_DEELEVATED
FunctionEnd
```

- [ ] **Step 5: Add the uninstaller's data question**

In `Section "Uninstall"`, after `RMDir "$INSTDIR"`:

```nsis
  ; The app's settings, profiles and logs. Default is to keep them, so reinstalling does
  ; not silently discard a user's keybinds and profiles. SetShellVarContext current here
  ; on purpose: this is the running user's data, not the machine's.
  SetShellVarContext current
  ${If} ${FileExists} "$LOCALAPPDATA\Wind\*.*"
    MessageBox MB_YESNO|MB_ICONQUESTION \
      "Remove Wind's settings, profiles and logs as well?$\n$\n$LOCALAPPDATA\Wind" \
      /SD IDNO IDNO keepData
    RMDir /r "$LOCALAPPDATA\Wind"
    keepData:
  ${EndIf}
```

`/SD IDNO` is what makes a silent uninstall keep the data rather than hang on the prompt.

- [ ] **Step 6: Rebuild and verify the new behaviour**

Run: `build.bat installer`
Expected: no warnings.

Then, with Wind running from a previous install:

```powershell
Start-Process 'C:\Program Files\Wind\Wind.exe'
Start-Sleep 2
Get-Process Wind | Select-Object Id
```

Run `dist\Wind-Setup-x64-0.1.0.exe`. Expected: the install completes without a "file in use" error, the previously running Wind is gone by the time files are copied, and after the finish page a new Wind is in the tray.

Then confirm the three new effects:

```powershell
Get-ItemProperty 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run' | Select-Object Wind
(Get-Process Wind).Path
Get-CimInstance Win32_Process -Filter "Name='Wind.exe'" |
  ForEach-Object { $_.GetOwner().User }
```

Expected: the Run value points at `C:\Program Files\Wind\Wind.exe`; the process path matches; and the owner is your own user account, not an administrator account. That last line is the one that proves `ShellExecAsUser` worked.

- [ ] **Step 7: Commit**

```bash
git add installer/app.nsh installer/wind.nsi installer/MicrosoftEdgeWebview2Setup.exe .gitignore
git commit -m "feat(installer): clean shutdown handshake, WebView2 bootstrap, autostart, de-elevated launch"
```

---

### Task 4: The release script

**Files:**
- Create: `tools/release.ps1`
- Modify: `README.md` (a Releases section describing the artifact and the signing switch)

**Interfaces:**
- Consumes: `build.bat`, `build.bat config`, `build.bat uiaccess`, `build.bat installer`.
- Produces: `dist/Wind-Setup-x64-<version>.exe`, signed when a certificate is configured.

- [ ] **Step 1: Write `tools/release.ps1`**

```powershell
<#
    Builds the release artifact: dist\Wind-Setup-x64-<version>.exe

    Signing is environment-driven, so no certificate detail ever enters the repository:
        $env:WIND_SIGN_THUMBPRINT   a cert in Cert:\CurrentUser\My or Cert:\LocalMachine\My
      or
        $env:WIND_SIGN_PFX          path to a .pfx
        $env:WIND_SIGN_PASSWORD     its password

    With a certificate this builds the uiAccess=true variant and signs it. Without one it
    builds the ordinary variant, because shipping a manifest that asks for a privilege
    Windows will refuse is noise in a public artifact. The app already degrades correctly:
    transform_model.cpp probes TokenUIAccess and disables the desktop transform pick.
#>
[CmdletBinding()]
param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Get-WindVersion {
    $h = Get-Content "$root\src\version.h" -Raw
    $maj = [regex]::Match($h, '#define\s+WIND_VER_MAJOR\s+(\d+)').Groups[1].Value
    $min = [regex]::Match($h, '#define\s+WIND_VER_MINOR\s+(\d+)').Groups[1].Value
    $pat = [regex]::Match($h, '#define\s+WIND_VER_PATCH\s+(\d+)').Groups[1].Value
    if (-not $maj) { throw "could not read WIND_VER_MAJOR from src\version.h" }
    "$maj.$min.$pat"
}

function Get-SigningCert {
    if ($env:WIND_SIGN_THUMBPRINT) {
        foreach ($store in 'Cert:\CurrentUser\My', 'Cert:\LocalMachine\My') {
            $c = Get-ChildItem $store -ErrorAction SilentlyContinue |
                 Where-Object Thumbprint -eq $env:WIND_SIGN_THUMBPRINT
            if ($c) { return $c }
        }
        throw "WIND_SIGN_THUMBPRINT is set but no such certificate was found."
    }
    if ($env:WIND_SIGN_PFX) {
        if (-not (Test-Path $env:WIND_SIGN_PFX)) { throw "WIND_SIGN_PFX does not exist: $($env:WIND_SIGN_PFX)" }
        $pw = ConvertTo-SecureString $env:WIND_SIGN_PASSWORD -AsPlainText -Force
        return Get-PfxCertificate -FilePath $env:WIND_SIGN_PFX -Password $pw
    }
    $null
}

function Invoke-Sign {
    param([string]$Path, $Cert)
    # A timestamp is what keeps the signature valid after the certificate expires.
    $s = Set-AuthenticodeSignature -FilePath $Path -Certificate $Cert -HashAlgorithm SHA256 `
             -TimestampServer 'http://timestamp.digicert.com'
    if ($s.Status -ne 'Valid') { throw "signing $Path failed: $($s.Status) $($s.StatusMessage)" }
    Write-Host "signed $(Split-Path -Leaf $Path): $($s.Status)"
}

$version = Get-WindVersion
$cert = Get-SigningCert
$variant = if ($cert) { 'uiaccess' } else { '' }

Write-Host "Wind $version"
if ($cert) {
    Write-Host "signing with: $($cert.Subject)"
    Write-Host "variant: uiAccess=true"
} else {
    Write-Warning "no certificate configured (WIND_SIGN_THUMBPRINT / WIND_SIGN_PFX)."
    Write-Warning "unsigned build: UIAccess features disabled (elevated-window keybinds, desktopTransform)."
}

if (-not $SkipBuild) {
    Write-Host "=== building Wind.exe ($(if ($variant) { $variant } else { 'standard' })) ==="
    & cmd /c "`"$root\build.bat`" $variant"
    if ($LASTEXITCODE -ne 0) { throw "build.bat $variant failed" }

    Write-Host "=== building WindConfig.exe + ui\dist ==="
    & cmd /c "`"$root\build.bat`" config"
    if ($LASTEXITCODE -ne 0) { throw "build.bat config failed" }
}

foreach ($f in 'Wind.exe', 'WindConfig.exe') {
    if (-not (Test-Path "$root\$f")) { throw "missing build output: $f" }
}
if (-not (Test-Path "$root\ui\dist\index.html")) { throw "missing ui\dist (build.bat config)" }

# The payload is signed BEFORE makensis packs it: signing the installer does not sign
# what is inside it, and UIAccess is granted on Wind.exe's own signature.
if ($cert) {
    Invoke-Sign "$root\Wind.exe" $cert
    Invoke-Sign "$root\WindConfig.exe" $cert
}

Write-Host "=== compiling the installer ==="
& cmd /c "`"$root\build.bat`" installer"
if ($LASTEXITCODE -ne 0) { throw "build.bat installer failed" }

$out = "$root\dist\Wind-Setup-x64-$version.exe"
if (-not (Test-Path $out)) { throw "installer not produced: $out" }
if ($cert) { Invoke-Sign $out $cert }

$mb = [math]::Round((Get-Item $out).Length / 1MB, 1)
Write-Host ""
Write-Host "DONE  $out  ($mb MB)"
if (-not $cert) { Write-Host "      unsigned - see tools\release.ps1 for the signing switch" }
```

- [ ] **Step 2: Run it unsigned and confirm the warning and the artifact**

Run: `pwsh -File tools\release.ps1`
Expected: the two "unsigned build" warnings, then `DONE  ...\dist\Wind-Setup-x64-0.1.0.exe` with a size in MB.

- [ ] **Step 3: Run it signed, using the existing dev certificate, to prove the signing path**

The dev certificate `CN=Wind Dev Test Cert` already exists on this machine from `tools\uiaccess_setup.ps1`. It is not a real cert, but it exercises the whole signed branch:

```powershell
$t = (Get-ChildItem Cert:\LocalMachine\My | Where-Object Subject -eq 'CN=Wind Dev Test Cert' |
      Sort-Object NotAfter -Descending | Select-Object -First 1).Thumbprint
$env:WIND_SIGN_THUMBPRINT = $t
pwsh -File tools\release.ps1
```

Expected: `variant: uiAccess=true`, three `signed ...: Valid` lines, and a `DONE` artifact. The timestamp server call needs the network; if it is unavailable the script fails loudly, which is correct.

Then clear it so later tasks build unsigned: `Remove-Item Env:\WIND_SIGN_THUMBPRINT`

- [ ] **Step 4: Document it in `README.md`**

Add a `## Releases` section stating: the artifact name, that it installs per-machine to `C:\Program Files\Wind` and needs administrator rights, that autostart is offered during setup, and the two environment variables that enable signing. State plainly that unsigned builds lose elevated-window keybinds and `desktopTransform`, and that a free OV certificate is being sought from SignPath Foundation.

- [ ] **Step 5: Commit**

```bash
git add tools/release.ps1 README.md
git commit -m "feat(installer): release script with environment-driven signing"
```

---

### Task 5: The build gate

**Files:**
- Create: `tools/installer_check.ps1`
- Modify: `build.bat` (run the check from the `installer` target)

**Interfaces:**
- Consumes: `installer/wind.nsi`, `installer/over.nsh` (absent until Task 7; the rectangle check skips itself when the file does not exist), `dist/Wind-Setup-x64-<version>.exe`.
- Produces: a non-zero exit code on any failure.

- [ ] **Step 1: Write `tools/installer_check.ps1`**

```powershell
<#
    Build gate for the installer. Three things makensis cannot tell you:

      1. whether every media file the script packs actually exists (NSIS warns rather
         than errors on a missing File source, so a typo ships an empty installer),
      2. whether every rectangle the pages read is present in the generated over.nsh
         (an edit to over.html that renames a control compiles fine and then hit-tests
         against a rectangle of zero size),
      3. whether a silent install actually lands and a silent uninstall actually leaves.

    Note the limit: /S exercises the section, not the drawn UI, and the drawn UI is most
    of the code. docs\VERIFICATION.md carries the manual matrix that covers the rest.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$fail = @()

function Check { param([string]$What, [scriptblock]$Test)
    try {
        if (& $Test) { Write-Host "  ok    $What" }
        else { Write-Host "  FAIL  $What"; $script:fail += $What }
    } catch { Write-Host "  FAIL  $What ($_)"; $script:fail += $What }
}

Write-Host "installer check"

# --- 1. every File source exists ---------------------------------------------
$nsh = Get-ChildItem "$root\installer" -Filter *.nsh -ErrorAction SilentlyContinue
$sources = @("$root\installer\wind.nsi") + $nsh.FullName
foreach ($f in $sources) {
    foreach ($line in Get-Content $f) {
        # File "..\Wind.exe"   /   File /r "..\ui\dist\*.*"   /   File "/oname=..." "x.exe"
        $m = [regex]::Match($line, '^\s*File\s+(?:/r\s+)?(?:"/oname=[^"]*"\s+)?"([^"]+)"')
        if (-not $m.Success) { continue }
        $rel = $m.Groups[1].Value
        if ($rel -match '^\$') { continue }          # a runtime path, not a build-time one
        $path = Join-Path "$root\installer" $rel
        Check "File source exists: $rel" { Test-Path $path }
    }
}

# --- 2. every rectangle the pages read is generated ---------------------------
$over = "$root\installer\over.nsh"
if (Test-Path $over) {
    $defined = @{}
    foreach ($line in Get-Content $over) {
        $m = [regex]::Match($line, '^\s*!define\s+(O_[A-Z0-9_]+)\s')
        if ($m.Success) { $defined[$m.Groups[1].Value] = $true }
    }
    # Pages and the compositor refer to rectangles through the OAT / HITS macros, which
    # take the bare NAME and build O_<NAME>_X and friends from it.
    $used = New-Object System.Collections.Generic.HashSet[string]
    foreach ($f in $nsh.FullName) {
        foreach ($line in Get-Content $f) {
            foreach ($m in [regex]::Matches($line, '!insertmacro\s+(?:OAT|HITS)\s+([A-Z0-9_]+)')) {
                [void]$used.Add($m.Groups[1].Value)
            }
        }
    }
    foreach ($name in $used) {
        foreach ($axis in 'X', 'Y', 'W', 'H') {
            Check "rectangle defined: O_${name}_$axis" { $defined.ContainsKey("O_${name}_$axis") }
        }
    }
} else {
    Write-Host "  skip  rectangle check (over.nsh not generated yet)"
}

# --- 3. silent install and uninstall ------------------------------------------
$h = Get-Content "$root\src\version.h" -Raw
$version = '{0}.{1}.{2}' -f
    [regex]::Match($h, 'WIND_VER_MAJOR\s+(\d+)').Groups[1].Value,
    [regex]::Match($h, 'WIND_VER_MINOR\s+(\d+)').Groups[1].Value,
    [regex]::Match($h, 'WIND_VER_PATCH\s+(\d+)').Groups[1].Value
$setup = "$root\dist\Wind-Setup-x64-$version.exe"

if (Test-Path $setup) {
    $scratch = Join-Path $env:TEMP "wind-installer-check"
    if (Test-Path $scratch) { Remove-Item $scratch -Recurse -Force }

    # /D must be the last argument and unquoted, which is an NSIS rule, not a typo.
    Start-Process $setup -ArgumentList "/S", "/D=$scratch" -Wait
    Check "silent install placed Wind.exe"       { Test-Path "$scratch\Wind.exe" }
    Check "silent install placed WindConfig.exe" { Test-Path "$scratch\WindConfig.exe" }
    Check "silent install placed ui\dist"        { Test-Path "$scratch\ui\dist\index.html" }
    Check "ARP key written" {
        (Get-ItemProperty 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Wind' `
            -ErrorAction SilentlyContinue).DisplayVersion -eq $version
    }
    Check "Run value written" {
        (Get-ItemProperty 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run' `
            -ErrorAction SilentlyContinue).Wind -match 'Wind\.exe'
    }

    if (Test-Path "$scratch\Uninstall.exe") {
        # _?= keeps the uninstaller in place so Wait actually waits for it, which is
        # another NSIS rule: without it the uninstaller copies itself to TEMP and returns.
        Start-Process "$scratch\Uninstall.exe" -ArgumentList "/S", "_?=$scratch" -Wait
        Start-Sleep -Milliseconds 800
        Check "uninstall removed Wind.exe" { -not (Test-Path "$scratch\Wind.exe") }
        Check "uninstall removed the ARP key" {
            $null -eq (Get-ItemProperty 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Wind' `
                -ErrorAction SilentlyContinue)
        }
        Check "uninstall removed the Run value" {
            $null -eq (Get-ItemProperty 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run' `
                -ErrorAction SilentlyContinue).Wind
        }
    }
    if (Test-Path $scratch) { Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue }
} else {
    Write-Host "  skip  install smoke (no $setup)"
}

Write-Host ""
if ($fail.Count) { Write-Host "$($fail.Count) check(s) failed"; exit 1 }
Write-Host "all checks passed"; exit 0
```

- [ ] **Step 2: Run the gate against the Task 4 artifact**

Run: `pwsh -File tools\installer_check.ps1`
Expected: `ok` for every File source, `skip` for the rectangle check, `ok` for all six install/uninstall lines, and `all checks passed`. The silent install needs elevation, so run it from an elevated shell.

- [ ] **Step 3: Prove the gate catches a real fault**

Temporarily change `File "..\Wind.exe"` in `installer/wind.nsi` to `File "..\Wnid.exe"`, re-run the gate.
Expected: `FAIL  File source exists: ..\Wnid.exe` and exit code 1. Revert the typo.

- [ ] **Step 4: Chain it from `build.bat installer`**

Replace the last two lines of the `:installer` target so the compile is followed by the gate:

```bat
"%MAKENSIS%" /WX /V2 "%ROOT%installer\wind.nsi"
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\installer_check.ps1"
exit /b %errorlevel%
```

- [ ] **Step 5: Commit**

```bash
git add tools/installer_check.ps1 build.bat
git commit -m "test(installer): build gate for media, rectangles and silent install"
```

---

### Task 6: The frame pipeline and the placeholder loop

**Files:**
- Create: `installer/make-loop.mjs`
- Create: `installer/media/800/v/*.jpg` (generated: Prism's frames, copied in)
- Create: `installer/README.md`

**Interfaces:**
- Consumes: nothing.
- Produces: `installer/media/800/v/000.jpg ...`, and the frame count that Task 8 puts in `video.nsh` as `${FRAMES}`.

- [ ] **Step 1: Copy Prism's frames in as the placeholder**

```powershell
$src = 'C:\Users\Admin\Documents\Claude\Github\Prism\build\installer\media\800\v'
$dst = 'C:\Users\Admin\Documents\Claude\Github\Wind\installer\media\800\v'
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item "$src\*.jpg" $dst
(Get-ChildItem $dst -Filter *.jpg).Count
```

Expected: 379. That number is `${FRAMES}` in Task 8.

- [ ] **Step 2: Write `installer/make-loop.mjs`**

A port of Prism's `make-loop.cjs` to ESM, with the hardcoded clip window turned into arguments, because Wind's clip is not yet shot and its length is unknown.

```js
/**
 * Builds the video loop setup plays, from a source clip.
 *
 *     node installer/make-loop.mjs "C:\path\to\clip.mp4" [--start 0] [--len 16] [--fps 24]
 *
 * Out: installer/media/800/v/000.jpg ...
 *
 * Two things worth knowing:
 *
 *  - The clip does not loop, so we make it loop. The last K frames are crossfaded onto
 *    the first K, which leaves the last frame running into frame 0 with a smaller step
 *    than an ordinary frame-to-frame one.
 *
 *  - The install screen cannot animate: NSIS runs the section on the script thread, so
 *    nothing can call back into script while files are being written. That screen draws
 *    one frame and lets the progress bar carry the motion.
 */
import { execFileSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const MEDIA = path.join(HERE, 'media')

const argv = process.argv.slice(2)
const SRC = argv[0]
const opt = (name, dflt) => {
  const i = argv.indexOf(`--${name}`)
  return i >= 0 ? Number(argv[i + 1]) : dflt
}
const START = opt('start', 0)
const LEN = opt('len', 16)
const FPS = opt('fps', 24)
const K = opt('fade', 36)     // crossfaded frames, a second and a half at 24 fps
// One size for every display. The overlay stays per DPI so type is always sharp, but the
// footage is defocused motion: an 800 to 1440 upscale is invisible on it, and halving the
// payload is what buys a long loop at a sane download size.
const SIZES = [{ w: 800, h: 600, q: 62 }]

const run = (bin, args) => execFileSync(bin, args, { stdio: ['ignore', 'pipe', 'pipe'] })

if (!SRC || !fs.existsSync(SRC)) {
  console.error('usage: node installer/make-loop.mjs <clip> [--start s] [--len s] [--fps n] [--fade n]')
  process.exit(1)
}

for (const { w, h, q } of SIZES) {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'wind-loop-'))
  const raw = path.join(tmp, 'raw')
  fs.mkdirSync(raw)

  run('ffmpeg', ['-y', '-v', 'error', '-ss', String(START), '-t', String(LEN), '-i', SRC,
    '-vf', [`fps=${FPS}`, `scale=${w}:${h}:force_original_aspect_ratio=increase`, `crop=${w}:${h}`].join(','),
    path.join(raw, '%03d.png')])

  // However many frames the source really yielded: asking for one more than exists is the
  // difference between a loop and a crash.
  const have = fs.readdirSync(raw).length
  const n = have - K
  if (n < K * 2) throw new Error(`only ${have} frames available, need at least ${K * 3}`)

  const f = (i) => path.join(raw, `${String(i + 1).padStart(3, '0')}.png`)
  const vdir = path.join(MEDIA, String(w), 'v')
  fs.rmSync(vdir, { recursive: true, force: true })
  fs.mkdirSync(vdir, { recursive: true })

  for (let i = 0; i < n; i++) {
    const dst = path.join(vdir, `${String(i).padStart(3, '0')}.jpg`)
    if (i < K) {
      // out[i] = frame[n+i] fading out under frame[i] fading in, so the wrap from the last
      // frame back to the first is already in progress by the time it happens
      const pct = Math.round((100 * i) / K)
      const blend = path.join(tmp, 'b.png')
      run('magick', [f(n + i), f(i), '-define', `compose:args=${pct}`, '-compose', 'blend', '-composite', blend])
      run('magick', [blend, '-quality', String(q), '-sampling-factor', '4:2:0', '-strip', dst])
    } else {
      run('magick', [f(i), '-quality', String(q), '-sampling-factor', '4:2:0', '-strip', dst])
    }
  }

  fs.rmSync(tmp, { recursive: true, force: true })
  const bytes = fs.readdirSync(vdir).reduce((a, x) => a + fs.statSync(path.join(vdir, x)).size, 0)
  console.log(`${w}x${h}: ${n} frames of ${have} available, ${(bytes / 1e6).toFixed(1)} MB`)
  console.log(`   set FRAMES to ${n} and TICK to ${Math.round(1000 / FPS)} in installer/video.nsh`)
}
```

- [ ] **Step 3: Verify the script runs against a clip**

Re-encode a few seconds of the placeholder frames into a test clip and round-trip it:

```powershell
$v = 'installer\media\800\v'
ffmpeg -y -v error -framerate 24 -i "$v\%03d.jpg" -c:v libx264 -pix_fmt yuv420p "$env:TEMP\wind-loop-test.mp4"
node installer/make-loop.mjs "$env:TEMP\wind-loop-test.mp4" --len 15 --fps 24
```

Expected: a line reporting the frame count, the megabytes, and the `FRAMES` / `TICK` values.

Then restore the placeholder frames from Prism (Step 1), because the round trip re-encoded them.

- [ ] **Step 4: Write `installer/README.md`**

Cover, in the style of Prism's: what each file is, how to change the words (edit `over.html`, regenerate both overlay sets), how to change the clip (run `make-loop.mjs`, put the reported numbers in `video.nsh`), what it costs in megabytes, and the standing rule that `installer/media/` is generated and never hand-edited. Note that the current footage is Prism's, held as a placeholder until Wind's own clip exists.

- [ ] **Step 5: Commit**

The frames are `.gitignore`d as generated output, so this commit is the scripts and the docs.

```bash
git add installer/make-loop.mjs installer/README.md
git commit -m "feat(installer): frame-loop pipeline and installer README"
```

---

### Task 7: Wind's overlays

**Files:**
- Create: `installer/over.html`
- Create: `installer/make-over.mjs`
- Create: `installer/over.nsh` (generated)
- Create: `installer/media/960/o/*.png`, `installer/media/1440/o/*.png` (generated)

**Interfaces:**
- Consumes: nothing.
- Produces, for Tasks 8 and 9:
  - Overlay PNGs named `<screen>.png` and `<screen>-hot-<control>.png`, where screen is one of `welcome`, `setup`, `copy`, `done`.
  - `box-on.png` and `box-off.png`, the two checkbox states stamped at runtime.
  - `installer/over.nsh` defining `O_<SCREEN>_<CONTROL>_{X,Y,W,H}` in 640x480 units.
  - Control names per screen: `CAP_MIN`, `CAP_X` on all four; `NEXT` on welcome, setup and done; `BACK` on setup and done-less screens as listed below; `PATH` on setup; `TRACK` on copy; `OPT_AUTOSTART` / `BOX_AUTOSTART` on setup; `OPT_RUN` / `BOX_RUN` and `OPT_DESK` / `BOX_DESK` on done.

- [ ] **Step 1: Design the four screens**

This is UI work, so it goes through the design skills rather than being eyeballed: invoke `frontend-design` for the visual direction and `impeccable` for the states and hierarchy pass. Constraints that are not negotiable:

- No panel. Type sits on the picture, held up by weight and a shadow, so every pixel needs alpha. That is why `make-over.mjs` solves for alpha instead of capturing it.
- Anything whose value changes at runtime is left OUT of the art: the install path text, the progress fill, and every checkbox box. They leave a gap that NSIS paints into.
- Palette comes from Wind, not Prism. Take the mark from `assets/wind-badge.svg`.
- The copy, verbatim:
  - welcome: "Install Wind" / "A fullscreen magnifier that keeps tracking the mouse, even when games hide it." Button: "Install Wind".
  - setup: "Where Wind goes" / "Wind installs to Program Files so Windows will grant it the access its keyboard shortcuts and desktop zoom need." Path field (read-only look, no Browse button). Checkbox: "Start Wind when I sign in". Buttons: "Back", "Install".
  - copy: "Installing" / "This takes a few seconds." Progress trough.
  - done: "Wind installed" / no subtitle. Checkboxes: "Open Wind now", "Create desktop shortcut". Button: "Finish".
- No em-dashes and no `&mdash;`.

- [ ] **Step 2: Write `installer/over.html`**

Structure it exactly as Prism's does, because `make-over.mjs` depends on the contract: two same-size panes side by side, `#k` on black and `#w` on white; a `window.render(screen, hot, boxes)` that fills both; a `window.rects()` that returns every `[data-a]` element's bounding rectangle. The pane is 640x480 and the body is 1280x480.

The `data-a` attribute names the rectangle, and the name is what ends up in `over.nsh` as `O_<SCREEN>_<NAME>_X`. Renaming one here without regenerating is the exact failure the Task 5 rectangle check catches.

- [ ] **Step 3: Write `installer/make-over.mjs`**

A port of `make-over.cjs` with Electron replaced by Playwright, which is already a devDependency in `ui/package.json`. The alpha solve is unchanged and is the reason for the two panes:

```js
/**
 * Renders installer/over.html into the alpha overlays the installer composites over its
 * video loop, plus the rectangles NSIS needs to know about.
 *
 *     node installer/make-over.mjs 1440
 *     node installer/make-over.mjs 960
 *
 * Alpha is not captured, it is solved for. Each screen is rendered twice, once on black
 * and once on white; for a pixel of colour C at coverage a those give A = C*a and
 * B = C*a + (1-a), so a = 1 - (B - A) and C = A / a. That is exact for antialiased type,
 * soft shadows and the glow under a button, none of which a screenshot of a transparent
 * window reliably brings back on Windows.
 *
 * Out: installer/media/<size>/o/<screen>[-hot-<control>].png
 *      installer/over.nsh   rectangles, in 640x480 units
 */
import { chromium } from 'playwright'
import { execFileSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const OUT = path.join(HERE, 'media')
const WIDTH = Number(process.argv[2]) || 1440
const SCALE = WIDTH / 640
const DIR = String(WIDTH)
const SCREENS = ['welcome', 'setup', 'copy', 'done']

// which control is drawn hot on which screen, and so which crops we need twice
const HOT = {
  welcome: ['next', 'close', 'min'],
  setup: ['next', 'back', 'close', 'min'],
  copy: ['close', 'min'],
  done: ['next', 'close', 'min']
}

const magick = (args) => execFileSync('magick', args, { stdio: ['ignore', 'pipe', 'pipe'] })

/** A = over black, B = over white -> straight RGBA */
function solveAlpha(onBlack, onWhite, out) {
  // alpha = 1 - (white - black), per channel, taken on the green channel which carries
  // the most luma; colour = black / alpha, which magick does as a divide by the alpha.
  const tmp = path.join(path.dirname(out), '_a.png')
  magick([onWhite, onBlack, '-compose', 'minus', '-composite', '-negate',
          '-colorspace', 'gray', tmp])
  magick([onBlack, tmp, '-compose', 'copy-opacity', '-composite',
          '-channel', 'RGB', '-evaluate', 'multiply', '1', '+channel', out])
  fs.rmSync(tmp, { force: true })
}

const browser = await chromium.launch()
const page = await browser.newPage({
  viewport: { width: 1280 * SCALE, height: 480 * SCALE },
  deviceScaleFactor: 1
})
// The page is authored in 640x480 units; the scale is applied once, here, so every
// rectangle it reports is already in those units and needs no conversion.
await page.goto('file://' + path.join(HERE, 'over.html').replace(/\\/g, '/'))
await page.addStyleTag({ content: `html { zoom: ${SCALE} }` })

const odir = path.join(OUT, DIR, 'o')
fs.rmSync(odir, { recursive: true, force: true })
fs.mkdirSync(odir, { recursive: true })
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'wind-over-'))

const w = Math.round(640 * SCALE)
const h = Math.round(480 * SCALE)
const lines = [
  '; Generated by make-over.mjs from over.html. Do not edit: run',
  ';   node installer/make-over.mjs 1440',
  ';   node installer/make-over.mjs 960',
  '; Rectangles are in 640x480 units; the installer scales them to its window.',
  ''
]

async function shoot(name, screen, hot, boxes) {
  await page.evaluate(([s, ht, bx]) => window.render(s, ht, bx), [screen, hot, boxes])
  await page.waitForTimeout(60)
  const shot = path.join(tmp, 'shot.png')
  await page.screenshot({ path: shot, clip: { x: 0, y: 0, width: w * 2, height: h } })
  const a = path.join(tmp, 'k.png')
  const b = path.join(tmp, 'w.png')
  magick([shot, '-crop', `${w}x${h}+0+0`, '+repage', a])
  magick([shot, '-crop', `${w}x${h}+${w}+0`, '+repage', b])
  solveAlpha(a, b, path.join(odir, `${name}.png`))
}

for (let s = 0; s < SCREENS.length; s++) {
  const screen = SCREENS[s]
  await shoot(screen, s, null, 'none')
  for (const hot of HOT[screen]) await shoot(`${screen}-hot-${hot}`, s, hot, 'none')

  const rects = await page.evaluate(() => window.rects())
  for (const [name, r] of Object.entries(rects)) {
    const u = (n) => Math.round(n / SCALE)
    lines.push(`!define O_${screen.toUpperCase()}_${name}_X ${u(r.x)}`)
    lines.push(`!define O_${screen.toUpperCase()}_${name}_Y ${u(r.y)}`)
    lines.push(`!define O_${screen.toUpperCase()}_${name}_W ${u(r.width)}`)
    lines.push(`!define O_${screen.toUpperCase()}_${name}_H ${u(r.height)}`)
  }
}

// The two checkbox states, cut out once and stamped every frame. Three checkboxes across
// two screens have eight states between them; baking those would be dozens of overlays.
for (const [state, file] of [['on', 'box-on.png'], ['off', 'box-off.png']]) {
  await page.evaluate(([bx]) => window.render(3, null, bx), [state])
  await page.waitForTimeout(60)
  const rects = await page.evaluate(() => window.rects())
  const r = rects.BOX_RUN
  const shot = path.join(tmp, 'boxshot.png')
  await page.screenshot({ path: shot, clip: { x: 0, y: 0, width: w * 2, height: h } })
  const a = path.join(tmp, 'bk.png')
  const b = path.join(tmp, 'bw.png')
  magick([shot, '-crop', `${Math.round(r.width)}x${Math.round(r.height)}+${Math.round(r.x)}+${Math.round(r.y)}`, '+repage', a])
  magick([shot, '-crop', `${Math.round(r.width)}x${Math.round(r.height)}+${Math.round(r.x) + w}+${Math.round(r.y)}`, '+repage', b])
  solveAlpha(a, b, path.join(odir, file))
}

fs.writeFileSync(path.join(HERE, 'over.nsh'), lines.join('\n') + '\n')
fs.rmSync(tmp, { recursive: true, force: true })
await browser.close()

const bytes = fs.readdirSync(odir).reduce((n, x) => n + fs.statSync(path.join(odir, x)).size, 0)
console.log(`${DIR}: ${fs.readdirSync(odir).length} overlays, ${(bytes / 1e6).toFixed(1)} MB`)
```

- [ ] **Step 4: Generate both overlay sets**

```powershell
cd ui; npx playwright install chromium; cd ..
node installer/make-over.mjs 1440
node installer/make-over.mjs 960
```

Expected: two lines reporting the overlay count and size, `installer/over.nsh` written, and `installer/media/1440/o` plus `installer/media/960/o` populated.

- [ ] **Step 5: Verify the alpha is real, not opaque**

The single most likely failure here is a solve that returns fully opaque overlays, which look right in isolation and hide the whole video at runtime.

```powershell
magick identify -format "%[opaque]\n" installer\media\1440\o\welcome.png
magick convert installer\media\1440\o\welcome.png -alpha extract -format "%[fx:mean]\n" info:
```

Expected: `false` from the first, and a mean well below 1.0 from the second (most of the frame is transparent). An answer of `true` and `1` means the solve failed and the two panes captured identically.

- [ ] **Step 6: Commit**

```bash
git add installer/over.html installer/make-over.mjs installer/over.nsh
git commit -m "feat(installer): Wind's overlay art and the Playwright overlay renderer"
```

`installer/media/` stays ignored; `over.nsh` is committed because the NSIS build needs it and it is small.

---

### Task 8: The compositor

**Files:**
- Create: `installer/kit.nsh`
- Create: `installer/video.nsh`

**Interfaces:**
- Consumes: `installer/over.nsh` (Task 7), `installer/media/` (Tasks 6 and 7).
- Produces, for `installer/pages.nsh` (Task 9):
  - `Var Dpi`, `Var Dialog`, `Var ArtDir`, `Var MediaSize`, `Var Font`
  - `Var Screen`, `Var Frame`, `Var Hot`, `Var Leaving`, `Var Canvas`
  - `!insertmacro HIDE_WIZARD_BUTTONS`, `!insertmacro UNPACK_MEDIA`
  - `!insertmacro OAT <NAME>` (sets `$R0..$R3` to a scaled rectangle), `!insertmacro HITS <NAME> <OUT>`
  - `Function WindCanvas` (takes the dialog on the stack), `WindCanvasFree`, `WindPickOverlay`, `WindDraw`, `WindTick`, `WindClick`
  - `!define ART_W 640`, `!define ART_H 480`, `!define FRAMES 379`, `!define TICK 42`

- [ ] **Step 1: Port `kit.nsh`**

Copy `C:\Users\Admin\Documents\Claude\Github\Prism\build\installer\kit.nsh` to `installer/kit.nsh` and make exactly these changes:

- rename `prismGuiInit` to `windGuiInit` and the `MUI_CUSTOMFUNCTION_GUIINIT` define to match,
- change `${BUILD_RESOURCES_DIR}\installer\media\...` to `media\...`, since there is no electron-builder resource root and `wind.nsi` sits one level up from nothing (paths in NSIS are relative to the script being compiled),
- keep `ART_W 640` / `ART_H 480`, the DPI probe, the `timeBeginPeriod(1)` call, the GDI+ startup, the frame removal, the rounded region and the font unchanged. Each carries a comment explaining a bug it fixes; keep the comments.

The DPI branch picks `1440` at 180 or above and `960` below, which on Max's 225% display means 1440. The footage stays one size (`media\800\v`) regardless.

- [ ] **Step 2: Port `video.nsh`**

Copy Prism's `video.nsh` to `installer/video.nsh` and make exactly these changes:

- rename every `Prism*` function to `Wind*` (`PrismCanvas` to `WindCanvas`, and the same for `CanvasFree`, `PickOverlay`, `Draw`, `Tick`, `Click`),
- change the include to `!include "over.nsh"`,
- set `!define FRAMES` to the count Task 6 Step 1 reported (379 for the placeholder) and `!define TICK 42`,
- change the screen names in `WindPickOverlay` from `welcome/where/copy/done` to `welcome/setup/copy/done`,
- change the runtime-painted text on screen 1 from Prism's editable `$INSTDIR` to Wind's fixed path: still drawn with GDI into the `PATH` rectangle, but the source is the constant `${INSTALL_DIR}` expanded, and there is no `BROWSE` hit target,
- change the checkbox variables from `$WantMenu` / `$WantDesk` to `$WantAutostart` (screen 1), `$RunAfter` and `$WantDesktop` (screen 3), which Task 3 already declares,
- keep the two-DIB front/back buffer arrangement, the `WS_CLIPCHILDREN` set, the `SS_BITMAP` choice and their comments verbatim. Each documents a specific flash or flicker it prevents.

- [ ] **Step 3: Confirm it compiles before any page uses it**

Add `!include "kit.nsh"` and `!include "video.nsh"` to `installer/wind.nsi` (guarded, so the uninstaller pass does not resize a window it never draws):

```nsis
!ifndef BUILD_UNINSTALLER
  !include "kit.nsh"
  !include "video.nsh"
!endif
```

Run: `build.bat installer`
Expected: compiles with no warnings. The stock pages are still in use, so nothing looks different yet; this step only proves the compositor parses, links its System calls and finds every media file and rectangle.

- [ ] **Step 4: Commit**

```bash
git add installer/kit.nsh installer/video.nsh installer/wind.nsi
git commit -m "feat(installer): frameless window and the frame compositor"
```

---

### Task 9: The four screens

**Files:**
- Create: `installer/pages.nsh`
- Modify: `installer/wind.nsi` (replace the MUI page block with the custom pages)

**Interfaces:**
- Consumes: everything Tasks 3, 7 and 8 produce.
- Produces: the finished installer UI.

- [ ] **Step 1: Write `installer/pages.nsh`**

Ported from Prism's, with the "where" page replaced by "setup" and no Browse. The shared start and leave functions are unchanged:

```nsis
;
; Wind setup, the four screens.
;
; Each page is the same thing: an empty dialog, the canvas from video.nsh, and a timer.
; What differs is $Screen, which picks the overlay and decides what the clicks mean.
; There is not a single button control in this file.
;
; Prism has a "where it goes" page with a Browse button. Wind does not, and the omission
; is deliberate: UIAccess is only granted to a signed binary in a secure location, so an
; install to D:\Apps\Wind would silently disable the elevated-window keybinds and the
; desktop transform that the per-machine install exists to enable. The path is shown and
; the reason is given, which is more honest than a chooser whose wrong answers are quiet.
;

Function windPageStart
  ; A silent install still calls a custom page's creator, and nsDialogs::Show with nothing
  ; to show never returns: /S would hang here forever. Abort in a creator means "skip this
  ; page", which is exactly right.
  ${If} ${Silent}
    Abort
  ${EndIf}
  !insertmacro HIDE_WIZARD_BUTTONS
  nsDialogs::Create 1018
  Pop $Dialog
  ${If} $Dialog == error
    Abort
  ${EndIf}
  Push $Dialog
  Call WindCanvas
  Call WindPickOverlay
  Call WindDraw
  ${NSD_CreateTimer} WindTick ${TICK}
FunctionEnd

Function windPageLeave
  ${NSD_KillTimer} WindTick
  Call WindCanvasFree
FunctionEnd

; ---- 1. welcome --------------------------------------------------------------
Function windWelcomeCreate
  ${If} ${Silent}
    Abort
  ${EndIf}
  InitPluginsDir
  !insertmacro UNPACK_MEDIA
  StrCpy $Screen 0
  Call windPageStart
  nsDialogs::Show
FunctionEnd

; ---- 2. setup ----------------------------------------------------------------
Function windSetupCreate
  StrCpy $Screen 1
  Call windPageStart
  nsDialogs::Show
FunctionEnd

; ---- 3. copying --------------------------------------------------------------
; MUI owns this page, and the section runs on the script thread, so nothing can call back
; into script while files are being written. This screen draws one frame and hands the
; motion to the progress bar, which Windows paints for us.
Function windCopyShow
  ${If} ${Silent}
    Return
  ${EndIf}
  FindWindow $R4 "#32770" "" $HWNDPARENT
  GetDlgItem $R5 $R4 1004   ; progress bar
  GetDlgItem $0 $R4 1006    ; status line
  ShowWindow $0 ${SW_HIDE}
  GetDlgItem $0 $R4 1016    ; the log
  ShowWindow $0 ${SW_HIDE}
  GetDlgItem $0 $R4 1027    ; "show details"
  ShowWindow $0 ${SW_HIDE}
  !insertmacro HIDE_WIZARD_BUTTONS

  ; MUI sizes this dialog from its own template, which is smaller than our window
  IntOp $R2 ${ART_W} * $Dpi
  IntOp $R2 $R2 / 96
  IntOp $R3 ${ART_H} * $Dpi
  IntOp $R3 $R3 / 96
  System::Call 'user32::SetWindowPos(p $R4, p 0, i 0, i 0, i $R2, i $R3, i 0x14)'

  StrCpy $Screen 2
  Push $R4
  Call WindCanvas
  StrCpy $Frame 30
  Call WindPickOverlay
  Call WindDraw

  ; theme off, smooth on, Wind's palette, sat on the drawn trough and raised above canvas
  System::Call 'uxtheme::SetWindowTheme(p $R5, w "", w "")'
  System::Call 'user32::GetWindowLong(p $R5, i -16) i .r0'
  IntOp $0 $0 | 0x01        ; PBS_SMOOTH
  System::Call 'user32::SetWindowLong(p $R5, i -16, i $0)'
  SendMessage $R5 ${PBM_SETBKCOLOR} 0 ${TRACK_BG}
  SendMessage $R5 ${PBM_SETBARCOLOR} 0 ${TRACK_FG}
  !insertmacro OAT COPY_TRACK
  System::Call 'user32::SetWindowPos(p $R5, p 0, i $R0, i $R1, i $R2, i $R3, i 0x10)'
FunctionEnd

; ---- 4. done -----------------------------------------------------------------
Function windDoneCreate
  StrCpy $Screen 3
  Call windPageStart
  nsDialogs::Show
FunctionEnd

Function windDoneLeave
  Call windPageLeave
  ${If} $WantDesktop == 1
    SetShellVarContext all
    CreateShortcut "$DESKTOP\Wind.lnk" "$INSTDIR\Wind.exe"
  ${EndIf}
  ${If} $RunAfter == 1
    !insertmacro WIND_LAUNCH_DEELEVATED
  ${EndIf}
FunctionEnd
```

`${TRACK_BG}` and `${TRACK_FG}` are two BGR colour constants defined in `wind.nsi` next to the other product defines, taken from the palette Task 7 settled on. NSIS wants them in `0xBBGGRR` order, which is the reverse of a CSS hex triplet, and getting that backwards is a silent wrong-colour bug rather than an error.

- [ ] **Step 2: Replace the page block in `installer/wind.nsi`**

Swap the three `MUI_PAGE_*` inserts and the `MUI_FINISHPAGE_RUN` defines for:

```nsis
!include "pages.nsh"

Page custom windWelcomeCreate windPageLeave
Page custom windSetupCreate   windPageLeave
!define MUI_PAGE_CUSTOMFUNCTION_SHOW windCopyShow
!insertmacro MUI_PAGE_INSTFILES
Page custom windDoneCreate    windDoneLeave
```

and add `SetAutoClose true` at the end of `Section "Wind"`, so the install page walks on to the done screen by itself rather than waiting for a Next button that has been hidden since `.onGUIInit`.

Delete the now-unused `WindLaunch` function from Task 3 Step 4; `windDoneLeave` owns the launch.

- [ ] **Step 3: Build and look at it**

Run: `build.bat installer`
Expected: compiles clean, and the Task 5 rectangle check now runs for real rather than skipping.

Then run `dist\Wind-Setup-x64-0.1.0.exe` and check by eye:

- the window is frameless with rounded corners and centred,
- the loop plays smoothly and wraps without a visible jump,
- hovering Install, Back, minimise and close changes each one,
- dragging the caption strip moves the window,
- the setup screen shows `C:\Program Files\Wind` as drawn text,
- the autostart checkbox toggles and its state survives moving to the next screen,
- the progress bar sits on the drawn trough in Wind's colours,
- the done screen's two checkboxes toggle, and Finish launches Wind only when "Open Wind now" is ticked.

- [ ] **Step 4: Verify silent install still works**

The custom pages are the most likely thing to break `/S`, because a creator that shows a dialog with nothing in it never returns.

Run: `pwsh -File tools\installer_check.ps1` from an elevated shell.
Expected: `all checks passed`. If it hangs, a `${If} ${Silent} Abort` guard is missing from a page creator.

- [ ] **Step 5: Commit**

```bash
git add installer/pages.nsh installer/wind.nsi
git commit -m "feat(installer): the four custom-drawn screens"
```

---

### Task 10: Documentation and the manual matrix

**Files:**
- Modify: `docs/VERIFICATION.md`
- Modify: `CLAUDE.md`
- Modify: `installer/README.md`

- [ ] **Step 1: Add the manual matrix to `docs/VERIFICATION.md`**

`/S` exercises the section, not the drawn UI, and the drawn UI is most of the code. Record that limit and the cases it leaves to a person, each with what to do and what to expect:

1. Fresh install on a machine with no Wind: files, ARP entry, Run value, tray icon.
2. Upgrade with Wind running and zoomed: no "file in use" error, and the OS cursor is visible afterwards (which is what proves the clean-quit handshake ran rather than the kill).
3. Upgrade with the Settings window open: WindConfig closes, no orphan process.
4. Uninstall keeping data: `%LOCALAPPDATA%\Wind\magnifier.ini` survives.
5. Uninstall deleting data: it does not.
6. At 100% DPI and at 225%: window centred, art sharp, hit targets land where they look.
7. WebView2 absent: hard to stage, so at minimum confirm the branch is skipped on a machine that has it, and that the tray's Open Settings works after install.

- [ ] **Step 2: Add the installer to `CLAUDE.md`**

Under Commands, add `build.bat installer`. Add a short Installer paragraph to Architecture pointing at `installer/README.md` and the spec, and add one gotcha, since it is the class of thing that section exists for:

> THE INSTALLER IS ELEVATED, WHICH MAKES HKCU AND `%LOCALAPPDATA%` THE WRONG USER'S. An elevated process's HKCU is whichever hive the elevated token owns, so autostart goes in HKLM Run, and Wind is launched with `ShellExecAsUser` rather than `Exec`. A plain `Exec` gives Wind an admin token, and `ResolveIniPath()` then puts magnifier.ini, the profiles and the logs in the administrator's profile where the user never finds them.

Keep the file under its ~200 line budget; trim if needed.

- [ ] **Step 3: Finish `installer/README.md`**

Fill in the file table now that every file exists, and add the regeneration commands with their real arguments.

- [ ] **Step 4: Commit and open the PR**

```bash
git add docs/VERIFICATION.md CLAUDE.md installer/README.md
git commit -m "docs(installer): verification matrix, gotcha and file guide"
gh pr create --fill
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| Technology, NSIS choice | 2 |
| Layout | 2, 3, 6, 7, 8, 9 |
| How the picture works | 7 (alpha solve), 8 (compositor) |
| The four screens | 7 (art and copy), 9 (behaviour) |
| Elevation, the two traps | 3 |
| Stopping the running instance | 3 |
| WebView2 | 1 (rule), 3 (implementation) |
| Signing | 4, plus the LICENSE in 2 |
| Uninstall | 2 (files, ARP), 3 (data question) |
| Version as one source of truth | 2 (`!searchparse`), 4 (`Get-WindVersion`) |
| Media | 6 (frames), 7 (overlays) |
| Testing | 1 (doctests), 5 (gate), 10 (matrix) |

No spec section is unimplemented.

**Known gaps, deliberate:** the WebView2-absent branch has no automated test because staging a machine without the runtime is not worth the effort for a branch that fires on a small minority; the rule it depends on is unit-tested instead. `over.html`'s visual design is specified by constraint and copy rather than by markup, because it goes through the design skills in Task 7 Step 1 and prescribing the CSS here would pre-empt that.
