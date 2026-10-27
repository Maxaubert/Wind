; Copyright (c) 2026 Max Aubert. All rights reserved.
; Proprietary and confidential source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
;
; Wind setup.
;
; Per-machine, elevated, into Program Files, because UIAccess is only granted to a signed
; binary in a secure location. The path is therefore not the user's to change: installing
; to D:\Apps\Wind would silently disable the features this install mode exists to enable.
;
; The picture (kit.nsh, video.nsh, screens.nsh) is layered on top of this; nothing in the
; sections below changes when it is.
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
!define ARP_KEY      "Software\Microsoft\Windows\CurrentVersion\Uninstall\Wind"
!define RUN_KEY      "Software\Microsoft\Windows\CurrentVersion\Run"
!define RUN_VALUE    "Wind"

; The progress bar's two colours, in NSIS's 0xBBGGRR order, which is the REVERSE of a CSS
; hex triplet. Getting it backwards is a wrong-colour bug, not an error.
!define TRACK_BG 0x261B1B   ; CSS #1b1b26, the trough drawn in over.html
!define TRACK_FG 0xD65B5B   ; CSS #5b5bd6, Wind's accent (--accent in ui/src/theme.css)

Name "${PRODUCT}"
OutFile "..\dist\Wind-Setup-x64-${WIND_VERSION}.exe"
InstallDir "$PROGRAMFILES64\Wind"
RequestExecutionLevel admin
ShowInstDetails hide
ShowUninstDetails hide

VIProductVersion "${WIND_VER_MAJOR}.${WIND_VER_MINOR}.${WIND_VER_PATCH}.0"
VIAddVersionKey "ProductName"     "${PRODUCT}"
VIAddVersionKey "FileDescription" "Wind setup"
VIAddVersionKey "FileVersion"     "${WIND_VERSION}"
VIAddVersionKey "ProductVersion"  "${WIND_VERSION}"
VIAddVersionKey "CompanyName"     "${PUBLISHER}"
VIAddVersionKey "LegalCopyright"  "Copyright (c) 2026 ${PUBLISHER}. All rights reserved."

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "app.nsh"

; UIAccess for everyone (issue #261). A release with a real code-signing certificate ships
; Wind.exe already signed as the uiAccess build, and there is nothing to do. Without one,
; tools\release.ps1 also builds the uiAccess variant as build\WindUA.exe, and setup signs it on the
; PC it installs to (local-sign.ps1 says how, and why that is safe). Its presence decides.
!if /FileExists "..\build\WindUA.exe"
  !define LOCAL_SIGN
!endif

; The picture. These come before MUI_LANGUAGE, because kit.nsh sets
; MUI_CUSTOMFUNCTION_GUIINIT and MUI only reads that when it emits .onGUIInit. Unlike Prism
; there is no BUILD_UNINSTALLER guard: electron-builder compiles the uninstaller in a second
; pass, plain NSIS emits both from one, and the uninstaller simply never calls any of this.
!include "kit.nsh"
!include "video.nsh"

!define MUI_ICON   "..\assets\wind.ico"
!define MUI_UNICON "..\assets\wind.ico"

!include "screens.nsh"

; Two page sets, one per run (issue #513). WIND_PICK_PLAIN in .onInit chooses: the accessible
; stock pages (plain.nsh) for a screen reader, high contrast or /ACCESSIBLE, otherwise the
; picture. Each page skips itself when its set is not the one running (the plain MUI pages
; through windPlainOnly as their PRE, the picture's creators through their own $Plain check),
; and NSIS walks past a skipped page in either direction, so Back works in both sets.
;
; Plain: welcome, licence, setup.
!define MUI_PAGE_CUSTOMFUNCTION_PRE windPlainOnly
!define MUI_WELCOMEPAGE_TEXT "Setup will install Wind, a fullscreen magnifier that keeps tracking the mouse, even when a game hides it.$\r$\n$\r$\nWind installs for everyone on this PC.$\r$\n$\r$\nClick Next to continue."
!insertmacro MUI_PAGE_WELCOME
!define MUI_PAGE_CUSTOMFUNCTION_PRE windPlainOnly
!define MUI_LICENSEPAGE_CHECKBOX
!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
Page custom windPlainSetupCreate windPlainSetupLeave

; The picture: welcome, licence, setup. Only its copying page is MUI's, because it is the one that
; has to be driven by the section; the SHOW define has to sit immediately before it, which is
; the only way to hand that page a show function without an earlier page swallowing it.
Page custom windWelcomeCreate windPageLeave
Page custom windLicenceCreate windLicenceLeave
Page custom windSetupCreate   windPageLeave

; Copying, shared: windCopyShow dresses it as the picture, and leaves it stock in plain mode.
!define MUI_PAGE_CUSTOMFUNCTION_SHOW windCopyShow
!insertmacro MUI_PAGE_INSTFILES

; Done. Plain: MUI's finish page, its two boxes standing in for the picture's. NOAUTOCLOSE, or
; MUI's own init would SetAutoClose true for both sets and walk past the install log; the
; picture's section sets autoclose itself.
!define MUI_PAGE_CUSTOMFUNCTION_PRE windPlainOnly
!define MUI_PAGE_CUSTOMFUNCTION_SHOW windPlainFinishShow
!define MUI_FINISHPAGE_NOAUTOCLOSE
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "&Open Wind now"
!define MUI_FINISHPAGE_RUN_FUNCTION windPlainRun
!define MUI_FINISHPAGE_SHOWREADME
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Create &desktop shortcut"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION windPlainDesktop
!insertmacro MUI_PAGE_FINISH
; The picture.
Page custom windDoneCreate    windDoneLeave

!include "plain.nsh"

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; Setup writes nothing to %LOCALAPPDATA%\Wind: the app seeds and owns that directory,
; including magnifier.ini, which it resolves through wind::ResolveIniPath().
Section "Wind" SEC_WIND
  ; Plain mode (issue #513) shows the log as it goes: NVDA reads each new line, and the
  ; picture's hidden log is the reason it was silent.
  ${If} $Plain = 1
    SetDetailsView show
  ${EndIf}
  !insertmacro WIND_QUIT_RUNNING

  SetOutPath "$INSTDIR"
!ifdef LOCAL_SIGN
  ; UIAccess needs a signature the PC trusts, so the uiAccess build is signed here, on this PC.
  ; 64-bit PowerShell through Sysnative: this installer is a 32-bit process, and the
  ; LocalMachine certificate stores are the machine's either way, but the PKI cmdlets are only
  ; guaranteed in the native one.
  ;
  ; The standard build goes in FIRST, and the uiAccess build is signed in $INSTDIR\.stage and only
  ; copied over it once its signature verifies. An unsigned uiAccess build does not start at
  ; all ("A referral was returned from the server"), so setup must never leave one in place:
  ; killed mid-signing, it did exactly that (reproduced 2026-09-28). This way an interruption
  ; at any point leaves a Wind that runs.
  File "..\build\Wind.exe"
  File "..\build\WindConfig.exe"
  File "..\build\WindTray.exe"
  ; Staged under $INSTDIR, not in $PLUGINSDIR: that is the user's %TEMP%, writable by a
  ; non-elevated process, and this step runs a script and signs a binary as admin. Program
  ; Files is admin-only, so nothing unprivileged can swap either between copy and use.
  RMDir /r "$INSTDIR\.stage"
  CreateDirectory "$INSTDIR\.stage\ua"
  File "/oname=$INSTDIR\.stage\ua\Wind.exe" "..\build\WindUA.exe"
  File "/oname=$INSTDIR\.stage\local-sign.ps1" "local-sign.ps1"
  DetailPrint "Signing Wind on this PC..."
  nsExec::ExecToLog '"$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\.stage\local-sign.ps1" -Stage "$INSTDIR\.stage\ua\Wind.exe" -Dir "$INSTDIR"'
  Pop $0
  ${If} $0 == 0
    ClearErrors
    CopyFiles /SILENT "$INSTDIR\.stage\ua\Wind.exe" "$INSTDIR\Wind.exe"
    ${If} ${Errors}
      DetailPrint "Could not place the signed build; keeping the standard one."
      File "..\build\Wind.exe"
      StrCpy $SignFailed 1
    ${EndIf}
  ${Else}
    DetailPrint "Local signing failed ($0); keeping the standard build."
    StrCpy $SignFailed 1
  ${EndIf}
  RMDir /r "$INSTDIR\.stage"
  ; The standard build has no UIAccess, and Wind refuses to start without it (issue #460), so
  ; say so here rather than leave a Wind that shows "Wind can't start" on its first launch.
  ${If} $SignFailed == 1
    StrCpy $RunAfter 0
    MessageBox MB_ICONSTOP|MB_OK "Setup couldn't sign Wind on this PC, so Windows won't give it UI Access, and Wind can't start without it.$\r$\n$\r$\nRun the Wind installer again. If this keeps happening, check that security software isn't blocking PowerShell while Setup runs." /SD IDOK
  ${EndIf}
!else
  File "..\build\Wind.exe"
  File "..\build\WindConfig.exe"
  File "..\build\WindTray.exe"
!endif
  ; the licence the user accepted, kept next to the app so the terms are always findable
  File "/oname=LICENSE.txt" "..\LICENSE"
  ; the open-source notices for what ships inside (Svelte, the xBR and NIS shaders, WebView2)
  File "/oname=THIRD-PARTY-NOTICES.txt" "..\THIRD-PARTY-NOTICES.md"
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

  !insertmacro WIND_ENSURE_WEBVIEW2
  ; $WantAutostart is chosen on the setup screen, which runs BEFORE this section, so it can be
  ; applied here. $WantDesktop and $RunAfter are chosen on the done screen, which runs after,
  ; and windDoneLeave applies those.
  !insertmacro WIND_APPLY_AUTOSTART

  ; The done screen is walked to by autoclose: the Next button it would otherwise wait for has
  ; been hidden since .onGUIInit. Plain mode keeps its Next button and its log, so it stops
  ; here and waits for the user, like any stock installer.
  ${If} $Plain <> 1
    SetAutoClose true
    ; That walk is a page swap no click started, so freeze here as a click would (issue #263);
    ; the done screen thaws after its first frame. Last, so a failure above never leaves it on.
    ${IfNot} ${Silent}
      !insertmacro WIND_FREEZE
    ${EndIf}
  ${EndIf}
SectionEnd

Section "Uninstall"
  !insertmacro WIND_QUIT_RUNNING

  SetShellVarContext all
  Delete "$SMPROGRAMS\Wind.lnk"
  Delete "$DESKTOP\Wind.lnk"
  DeleteRegValue HKLM "${RUN_KEY}" "${RUN_VALUE}"
  DeleteRegKey   HKLM "${ARP_KEY}"

  ; Retire the trust setup added for the local signature. Always, not only when this build
  ; signed: an older install may have, and a root left behind for an uninstalled app is litter.
  CreateDirectory "$INSTDIR\.stage"
  File "/oname=$INSTDIR\.stage\local-sign.ps1" "local-sign.ps1"
  nsExec::ExecToLog '"$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\.stage\local-sign.ps1" -Remove'
  Pop $0
  RMDir /r "$INSTDIR\.stage"

  Delete "$INSTDIR\Wind.exe"
  Delete "$INSTDIR\WindConfig.exe"
  Delete "$INSTDIR\WindTray.exe"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\THIRD-PARTY-NOTICES.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir /r "$INSTDIR\ui"
  ; Not RMDir /r on $INSTDIR itself: a stray recursive delete of the wrong directory is
  ; the one unrecoverable installer bug, so only the files we wrote are named.
  RMDir "$INSTDIR"

  ; The app's settings, profiles and logs. Default is to keep them, so reinstalling does
  ; not silently discard someone's keybinds and profiles. SetShellVarContext current here
  ; on purpose: this is the running user's data, not the machine's. /SD IDNO is what makes
  ; a silent uninstall keep the data rather than hang waiting on a prompt nobody can see.
  ; WHOSE data: the SIGNED-IN user's, which is not necessarily the elevated token's (issue
  ; #274). A standard user who elevated with an administrator's credentials has an
  ; elevated $LOCALAPPDATA in the ADMIN's profile. user-localappdata.ps1 asks the owner of
  ; this session's explorer.exe instead, and falls back to $LOCALAPPDATA if it cannot tell.
  SetShellVarContext current
  File "/oname=$PLUGINSDIR\user-localappdata.ps1" "user-localappdata.ps1"
  nsExec::ExecToStack '"$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\user-localappdata.ps1"'
  Pop $0
  Pop $R9
  ${If} $0 != 0
  ${OrIf} $R9 == ""
    StrCpy $R9 $LOCALAPPDATA
  ${EndIf}
  ${If} $R9 != ""
  ${AndIf} ${FileExists} "$R9\Wind\*.*"
    MessageBox MB_YESNO|MB_ICONQUESTION \
      "Remove Wind's settings, profiles and logs as well?$\n$\n$R9\Wind" \
      /SD IDNO IDNO keepData
    RMDir /r "$R9\Wind"
    keepData:
  ${EndIf}
SectionEnd

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Wind requires 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64

  ; Defaults for the three choices setup offers. The custom pages edit these; with the
  ; stock pages, and under /S, they are what actually applies.
  StrCpy $WantAutostart 1
  StrCpy $WantDesktop 0
  StrCpy $RunAfter 1
  StrCpy $SignFailed 0
  ; the licence starts unaccepted; only the licence screen's box sets it
  StrCpy $Accepted 0
  ; which page set runs: the picture, or the accessible stock pages (kit.nsh)
  !insertmacro WIND_PICK_PLAIN
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd
