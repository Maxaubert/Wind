;
; Wind setup.
;
; Per-machine, elevated, into Program Files, because UIAccess is only granted to a signed
; binary in a secure location. The path is therefore not the user's to change: installing
; to D:\Apps\Wind would silently disable the features this install mode exists to enable.
;
; The picture (kit.nsh, video.nsh, pages.nsh) is layered on top of this; nothing in the
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
!define TRACK_BG 0x2A2320   ; CSS #20232a
!define TRACK_FG 0xE8A15B   ; CSS #5ba1e8

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
VIAddVersionKey "LegalCopyright"  "Copyright (c) 2026 ${PUBLISHER}"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "app.nsh"

!define MUI_ICON   "..\assets\wind.ico"
!define MUI_UNICON "..\assets\wind.ico"

; Stock pages for now. pages.nsh replaces this block once the picture exists.
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION WindLaunch
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function WindLaunch
  !insertmacro WIND_LAUNCH_DEELEVATED
FunctionEnd

; Setup writes nothing to %LOCALAPPDATA%\Wind: the app seeds and owns that directory,
; including magnifier.ini, which it resolves through wind::ResolveIniPath().
Section "Wind" SEC_WIND
  !insertmacro WIND_QUIT_RUNNING

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

  !insertmacro WIND_ENSURE_WEBVIEW2
  !insertmacro WIND_APPLY_AUTOSTART
  ${If} $WantDesktop == 1
    CreateShortcut "$DESKTOP\Wind.lnk" "$INSTDIR\Wind.exe"
  ${EndIf}
SectionEnd

Section "Uninstall"
  !insertmacro WIND_QUIT_RUNNING

  SetShellVarContext all
  Delete "$SMPROGRAMS\Wind.lnk"
  Delete "$DESKTOP\Wind.lnk"
  DeleteRegValue HKLM "${RUN_KEY}" "${RUN_VALUE}"
  DeleteRegKey   HKLM "${ARP_KEY}"

  Delete "$INSTDIR\Wind.exe"
  Delete "$INSTDIR\WindConfig.exe"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir /r "$INSTDIR\ui"
  ; Not RMDir /r on $INSTDIR itself: a stray recursive delete of the wrong directory is
  ; the one unrecoverable installer bug, so only the files we wrote are named.
  RMDir "$INSTDIR"

  ; The app's settings, profiles and logs. Default is to keep them, so reinstalling does
  ; not silently discard someone's keybinds and profiles. SetShellVarContext current here
  ; on purpose: this is the running user's data, not the machine's. /SD IDNO is what makes
  ; a silent uninstall keep the data rather than hang waiting on a prompt nobody can see.
  SetShellVarContext current
  ${If} ${FileExists} "$LOCALAPPDATA\Wind\*.*"
    MessageBox MB_YESNO|MB_ICONQUESTION \
      "Remove Wind's settings, profiles and logs as well?$\n$\n$LOCALAPPDATA\Wind" \
      /SD IDNO IDNO keepData
    RMDir /r "$LOCALAPPDATA\Wind"
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
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd
