;
; Wind setup, part one: the window.
;
; There is no wizard here. NSIS's frame, header, hairline and three buttons are all removed,
; the window is sized to the picture, and everything you see after that is drawn by video.nsh
; into a single control.
;
; The caption is part of the picture too, which is why the window has no title bar: dragging
; is handled by hit-testing the top strip ourselves.
;
; Ported from Prism's installer. The comments are kept because each one records a bug it
; prevents, not a preference.
;

!include "LogicLib.nsh"
!include "nsDialogs.nsh"
!include "WinMessages.nsh"

!define ART_W 640
!define ART_H 480

!define /ifndef SW_HIDE 0
!define K_LOGPIXELSX 88
!define K_SWP_NOACT 0x0010
!define K_SWP_FRAME 0x0027      ; FRAMECHANGED|NOMOVE|NOSIZE|NOZORDER

Var Dpi         ; 96 at 100%, 216 at 225%
Var MediaSize   ; which set of overlays suits this display
Var ArtDir
Var Dialog
Var Gdip        ; GDI+ token, started once and never stopped: setup outlives it
Var Font

!macro HIDE_CHILD ID
  GetDlgItem $0 $HWNDPARENT ${ID}
  ${If} $0 <> 0
    ShowWindow $0 ${SW_HIDE}
  ${EndIf}
!macroend

; NSIS puts Back, Next and Cancel back on every page it owns, so this runs on each one
; rather than once at startup.
!macro HIDE_WIZARD_BUTTONS
  !insertmacro HIDE_CHILD 1
  !insertmacro HIDE_CHILD 2
  !insertmacro HIDE_CHILD 3
!macroend

!define MUI_CUSTOMFUNCTION_GUIINIT windGuiInit

Function windGuiInit
  ; --- what are we drawing at? ------------------------------------------------------------
  System::Call 'user32::GetDpiForSystem() i .r0 ? e'
  Pop $1
  ${If} $0 < 72
    System::Call 'user32::GetDC(p 0) p .r1'
    System::Call 'gdi32::GetDeviceCaps(p $1, i ${K_LOGPIXELSX}) i .r0'
    System::Call 'user32::ReleaseDC(p 0, p $1)'
  ${EndIf}
  ${If} $0 < 72
    StrCpy $0 96
  ${EndIf}
  StrCpy $Dpi $0
  ; Pick the overlay set whose pixels are the size of the window, so the type is drawn one to
  ; one. The footage is a single size for every display: it is defocused motion, and an
  ; upscale is invisible on it.
  ${If} $Dpi >= 180
    StrCpy $MediaSize "1440"
  ${Else}
    StrCpy $MediaSize "960"
  ${EndIf}

  ; A default timer only fires on the 15.6 ms scheduler tick, which turns a 42 ms request
  ; into 47 and the frame rate into a stutter. One millisecond, please.
  System::Call 'winmm::timeBeginPeriod(i 1)'

  ; --- GDI+, which decodes every frame and does the alpha ----------------------------------
  System::Call '*(i 1, p 0, i 0, i 0) p .r0'
  System::Call 'gdiplus::GdiplusStartup(*p .r1, p $0, p 0) i'
  System::Free $0
  StrCpy $Gdip $1

  ; --- take the frame off -------------------------------------------------------------------
  System::Call 'user32::GetWindowLong(p $HWNDPARENT, i -16) i .r0'
  IntOp $0 $0 & 0xFF30FFFF      ; ~(WS_CAPTION|SYSMENU|THICKFRAME|MIN|MAXBOX)
  IntOp $0 $0 | 0x02000000      ; WS_CLIPCHILDREN: never paint under the page
  System::Call 'user32::SetWindowLong(p $HWNDPARENT, i -16, i $0)'
  System::Call 'user32::GetWindowLong(p $HWNDPARENT, i -20) i .r0'
  IntOp $0 $0 | 0x40000         ; WS_EX_APPWINDOW: keep the taskbar button
  System::Call 'user32::SetWindowLong(p $HWNDPARENT, i -20, i $0)'
  System::Call 'user32::SetWindowPos(p $HWNDPARENT, p 0, i 0, i 0, i 0, i 0, i ${K_SWP_FRAME})'

  !insertmacro HIDE_WIZARD_BUTTONS
  !insertmacro HIDE_CHILD 1028   ; branding strip
  !insertmacro HIDE_CHILD 1256   ; hairline
  !insertmacro HIDE_CHILD 1034   ; header plate
  !insertmacro HIDE_CHILD 1035
  !insertmacro HIDE_CHILD 1036
  !insertmacro HIDE_CHILD 1037
  !insertmacro HIDE_CHILD 1038
  !insertmacro HIDE_CHILD 1039

  ; --- size it to the picture and centre it on the work area --------------------------------
  IntOp $R2 ${ART_W} * $Dpi
  IntOp $R2 $R2 / 96
  IntOp $R3 ${ART_H} * $Dpi
  IntOp $R3 $R3 / 96

  System::Call '*(i,i,i,i) p .r9'
  System::Call 'user32::SystemParametersInfo(i 0x30, i 0, p $9, i 0)'
  System::Call '*$9(i .r0, i .r1, i .r2, i .r3)'
  System::Free $9
  IntOp $2 $2 - $0
  IntOp $3 $3 - $1
  IntOp $2 $2 - $R2
  IntOp $2 $2 / 2
  IntOp $2 $2 + $0
  IntOp $3 $3 - $R3
  IntOp $3 $3 / 2
  IntOp $3 $3 + $1
  System::Call 'user32::SetWindowPos(p $HWNDPARENT, p 0, i $2, i $3, i $R2, i $R3, i ${K_SWP_NOACT})'

  ; rounded corners, since there is no frame left to round them for us
  IntOp $0 $R2 + 1
  IntOp $1 $R3 + 1
  IntOp $4 14 * $Dpi
  IntOp $4 $4 / 96
  System::Call 'gdi32::CreateRoundRectRgn(i 0, i 0, i $0, i $1, i $4, i $4) p .r5'
  System::Call 'user32::SetWindowRgn(p $HWNDPARENT, p $5, i 1)'

  ; the inner dialog is the whole window now
  GetDlgItem $0 $HWNDPARENT 1018
  System::Call 'user32::SetWindowPos(p $0, p 0, i 0, i 0, i $R2, i $R3, i ${K_SWP_NOACT})'

  ; --- the one font setup writes with --------------------------------------------------------
  ; Consolas, because the only string NSIS paints itself is the install path, and a path in a
  ; monospace face says what it is. over.html sets the same expectation around the gap it
  ; leaves, so the drawn text and the art have to agree.
  IntOp $0 12 * $Dpi
  IntOp $0 $0 / 96
  IntOp $0 0 - $0
  System::Call 'gdi32::CreateFont(i $0, i 0, i 0, i 0, i 400, i 0, i 0, i 0, \
    i 1, i 0, i 0, i 5, i 0, t "Consolas") p .r1'
  StrCpy $Font $1
FunctionEnd

; The loop and the overlays ride along inside setup, and the set that matches this display is
; unpacked before the first page draws.
!macro UNPACK_MEDIA
  StrCpy $ArtDir "$PLUGINSDIR\m"
  CreateDirectory "$ArtDir\v"
  CreateDirectory "$ArtDir\o"
  ; The footage is one size for every display. Only the type changes with the DPI.
  SetOutPath "$ArtDir\v"
  File "media\800\v\*.jpg"
  SetOutPath "$ArtDir\o"
  ${If} $MediaSize == "1440"
    File "media\1440\o\*.png"
  ${Else}
    File "media\960\o\*.png"
  ${EndIf}
!macroend
