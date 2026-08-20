;
; Wind setup, the four screens.
;
; Each page is the same thing: an empty dialog, the canvas from video.nsh, and a timer. What
; differs is $Screen, which picks the overlay and decides what the clicks mean. There is not a
; single button control in this file.
;
; Prism has a "where it goes" page with a Browse button. Wind does not, and the omission is
; deliberate: UIAccess is only granted to a signed binary in a secure location, so an install
; to D:\Apps\Wind would silently disable the elevated-window shortcuts and the desktop
; transform that the per-machine install exists to enable. The path is shown and the reason is
; given, which is more honest than a chooser whose wrong answers are quiet.
;

Function windPageStart
  ; A silent install still calls a custom page's creator, and nsDialogs::Show with nothing to
  ; show never returns: /S would hang here forever. Abort in a creator means "skip this page",
  ; which is exactly right.
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
; MUI owns this page, and the section runs on the script thread, so nothing can call back into
; script while files are being written. This screen draws one frame and hands the motion to
; the progress bar, which Windows paints for us.
Function windCopyShow
  ${If} ${Silent}
    Return
  ${EndIf}
  FindWindow $R4 "#32770" "" $HWNDPARENT
  GetDlgItem $R5 $R4 1004   ; progress bar
  GetDlgItem $0 $R4 1006    ; status line, which prints nothing here
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
  StrCpy $Frame 30          ; a frame with room under the words
  Call WindPickOverlay
  Call WindDraw

  ; the progress bar: theme off, smooth on, Wind's indigo, sat on the drawn trough and raised
  ; above the canvas
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

  ; Both of these are chosen on THIS screen, which runs after the section, so the section
  ; cannot act on them and they are applied here instead. ($WantAutostart is different: it is
  ; chosen on the setup screen, before the section, so the section applies it.)
  ${If} $WantDesktop == 1
    SetShellVarContext all
    CreateShortcut "$DESKTOP\Wind.lnk" "$INSTDIR\Wind.exe"
  ${EndIf}
  ${If} $RunAfter == 1
    !insertmacro WIND_LAUNCH_DEELEVATED
  ${EndIf}
FunctionEnd
