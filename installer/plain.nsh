; Copyright (c) 2026 Max Aubert. All rights reserved.
; Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
;
; Wind setup, the accessible plain pages (issue #513).
;
; The picture (kit.nsh, video.nsh, screens.nsh) is pixels: a screen reader finds nothing to name
; on it and the keyboard has nothing to focus. These pages are the stock alternative, real
; dialog controls with text, tab order and default buttons: MUI's welcome, licence (with a real
; checkbox), install and finish pages, plus one nsDialogs page for the autostart choice. They
; offer exactly what the picture offers. kit.nsh's WIND_PICK_PLAIN decides which set runs;
; wind.nsi lists both, each set skipping itself when the other is chosen.
;
; Included after the page list, because the finish page's show function reads
; $mui.FinishPage.Run, which MUI only declares when that page is inserted.
;

Var PlainAutostart  ; the setup page's checkbox

; PRE for every MUI page that belongs to plain mode only: skip it when the picture runs.
Function windPlainOnly
  ${If} $Plain <> 1
    Abort
  ${EndIf}
FunctionEnd

; ---- setup: where Wind goes, and autostart -----------------------------------
; The path is shown, not chosen, for the reason screens.nsh gives: UIAccess is only granted in a
; secure location.
Function windPlainSetupCreate
  ${If} ${Silent}
  ${OrIf} $Plain <> 1
    Abort
  ${EndIf}
  !insertmacro MUI_HEADER_TEXT "Where Wind goes" "Wind installs to Program Files."
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}
  ${NSD_CreateLabel} 0 0 100% 30u "Wind installs for everyone on this PC. Windows only grants the access its shortcuts and desktop zoom need to apps in a protected folder, so the folder cannot be changed."
  Pop $0
  ${NSD_CreateLabel} 0 36u 100% 12u "Install folder: $INSTDIR"
  Pop $0
  ${NSD_CreateCheckbox} 0 60u 100% 12u "&Start Wind when I sign in"
  Pop $PlainAutostart
  ${If} $WantAutostart = 1
    ${NSD_Check} $PlainAutostart
  ${EndIf}
  ; Back runs no leave function, so the box writes its choice as it changes, or Back and
  ; Next again would show the old state.
  ${NSD_OnClick} $PlainAutostart windPlainSetupClick
  ; NSIS only names the button Install on its own pages; this one starts the install too
  GetDlgItem $0 $HWNDPARENT 1
  SendMessage $0 ${WM_SETTEXT} 0 "STR:$(^InstallBtn)"
  ; focus on the one choice, so a screen reader announces it as the page opens
  ${NSD_SetFocus} $PlainAutostart
  nsDialogs::Show
FunctionEnd

; The box's click handler: nsDialogs pushes the control, so it is popped here.
Function windPlainSetupClick
  Pop $0
  Call windPlainSetupLeave
FunctionEnd

; Read before the section runs, which applies it (WIND_APPLY_AUTOSTART), as on the picture.
Function windPlainSetupLeave
  ${NSD_GetState} $PlainAutostart $0
  ${If} $0 = ${BST_CHECKED}
    StrCpy $WantAutostart 1
  ${Else}
    StrCpy $WantAutostart 0
  ${EndIf}
FunctionEnd

; ---- finish ------------------------------------------------------------------
; "Open Wind now" starts ticked, unless local signing failed: that Wind has no UIAccess and
; cannot start (the section has already said so and cleared $RunAfter), as on the picture.
Function windPlainFinishShow
  ${If} $RunAfter <> 1
    ${NSD_Uncheck} $mui.FinishPage.Run
  ${EndIf}
FunctionEnd

; MUI calls these on Finish when their box is ticked. Through explorer.exe, never Exec, so
; Wind does not inherit setup's admin token (app.nsh).
Function windPlainRun
  !insertmacro WIND_LAUNCH_DEELEVATED
FunctionEnd

Function windPlainDesktop
  SetShellVarContext all
  CreateShortcut "$DESKTOP\Wind.lnk" "$INSTDIR\Wind.exe"
FunctionEnd
