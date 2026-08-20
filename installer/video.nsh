;
; Wind setup, the picture: a video loop with the UI composited over it.
;
; NSIS has no video control, so setup plays the loop itself. Each tick it decodes one JPEG
; through GDI+, alpha-blends two overlays on top, writes the two things that change at
; runtime (the install path, the checkbox states), and asks one static control to repaint.
; Everything lands in a single DIB, so nothing flickers and there are no sibling controls to
; fight over z-order.
;
; TWO overlays, not one: back.png carries the shade and the caption scrim,
; which are identical on every screen, and the per-screen overlay carries only type and
; controls. Baking them together stored the same full-frame gradient nineteen times over and
; measured 28.3 MB of media against 13.3 MB for the split.
;
; It also means setup has no clickable controls at all. The same tick asks Windows where the
; pointer is and whether its button just went down, which is how hover, clicks and dragging a
; frameless window all work here.
;
; Ported from Prism's installer. The comments come with it: each one records a bug it
; prevents.
;

!include "over.nsh"

!define FRAMES 324         ; 13.5 seconds at 24 fps, the blue window of the source clip
!define TICK   42

!define /ifndef SRCCOPY 0x00CC0020
!define /ifndef TRANSPARENT 1
!define /ifndef WM_NCLBUTTONDOWN 0x00A1
!define /ifndef HTCAPTION 2
!define /ifndef STM_SETIMAGE 0x0172
!define /ifndef IMAGE_BITMAP 0

Var MemDC
Var DibBmp      ; the back buffer, which only we ever touch
Var DibBits     ; ...and a pointer straight at its pixels
Var DibShown    ; the front buffer, which only the static ever touches
Var ShownBits
Var DibBytes
Var OldBmp
Var Canvas      ; the one static that shows everything
Var Frame
Var Screen      ; 0 welcome, 1 setup, 2 copying, 3 done
Var OverName    ; which overlay is loaded, so we only reload on a change
Var OverImg
Var BackImg     ; the shade and caption scrim, loaded once per page
Var Hot         ; control under the pointer
Var WasDown
Var Leaving     ; an action is posted; this page is finished
Var BoxOn       ; the two states of a checkbox, stamped rather than baked
Var BoxOff
Var CanvasW
Var CanvasH

; ---- geometry ----------------------------------------------------------------
; over.nsh rectangles are 640x480 units; the window is whatever the display says.
!macro OAT NAME
  IntOp $R0 ${O_${NAME}_X} * $Dpi
  IntOp $R0 $R0 / 96
  IntOp $R1 ${O_${NAME}_Y} * $Dpi
  IntOp $R1 $R1 / 96
  IntOp $R2 ${O_${NAME}_W} * $Dpi
  IntOp $R2 $R2 / 96
  IntOp $R3 ${O_${NAME}_H} * $Dpi
  IntOp $R3 $R3 / 96
!macroend

; is the pointer ($6,$7 in client pixels) inside NAME?
!macro HITS NAME OUT
  !insertmacro OAT ${NAME}
  StrCpy ${OUT} 0
  IntOp $8 $R0 + $R2
  IntOp $9 $R1 + $R3
  ${If} $6 >= $R0
  ${AndIf} $6 < $8
  ${AndIf} $7 >= $R1
  ${AndIf} $7 < $9
    StrCpy ${OUT} 1
  ${EndIf}
!macroend

; ---- the canvas --------------------------------------------------------------
; One 32-bit DIB, one memory DC, one static. Created once per page.
Function WindCanvas
  Pop $0   ; the dialog to put it in

  IntOp $CanvasW ${ART_W} * $Dpi
  IntOp $CanvasW $CanvasW / 96
  IntOp $CanvasH ${ART_H} * $Dpi
  IntOp $CanvasH $CanvasH / 96

  ; BITMAPINFOHEADER, top-down (negative height) so it matches how images decode. biPlanes
  ; and biBitCount are WORDs: writing them as ints shifts every field after them and
  ; CreateDIBSection quietly returns nothing.
  ; Two buffers, and the front one is never selected into a DC by us. We draw into the back
  ; buffer, then copy its pixels straight into the front buffer's memory. That avoids two
  ; separate causes of the white flash: a bitmap we hold selected cannot be painted by the
  ; static, and STM_SETIMAGE invalidates the control *with erase*, so the dialog background
  ; is painted before every frame.
  IntOp $1 0 - $CanvasH
  System::Call '*(i 40, i $CanvasW, i $1, &i2 1, &i2 32, i 0, i 0, i 0, i 0, i 0, i 0) p .r2'
  System::Call 'gdi32::CreateDIBSection(p 0, p $2, i 0, *p .r3, p 0, i 0) p .r4'
  StrCpy $DibBmp $4
  StrCpy $DibBits $3
  System::Call 'gdi32::CreateDIBSection(p 0, p $2, i 0, *p .r3, p 0, i 0) p .r4'
  StrCpy $DibShown $4
  StrCpy $ShownBits $3
  System::Free $2
  IntOp $DibBytes $CanvasW * $CanvasH
  IntOp $DibBytes $DibBytes * 4
  System::Call 'gdi32::CreateCompatibleDC(p 0) p .r5'
  StrCpy $MemDC $5
  System::Call 'gdi32::SelectObject(p $MemDC, p $DibBmp) p .r8'
  StrCpy $OldBmp $8
  System::Call 'gdi32::SelectObject(p $MemDC, p $OldBmp)'

  ; The white flash is the dialog erasing its own face colour underneath us: a saved frame of
  ; it was the whole client area in #F0F0F0. WS_CLIPCHILDREN forbids a parent from painting
  ; where a child already covers, which here is everywhere.
  System::Call 'user32::GetWindowLong(p $0, i -16) i .r6'
  IntOp $6 $6 | 0x02000000
  System::Call 'user32::SetWindowLong(p $0, i -16, i $6)'

  ; Plain SS_BITMAP rather than SS_REALSIZECONTROL: the bitmap is already the control's exact
  ; size, so it is blitted instead of erased-then-stretched, and stretching a million pixels
  ; takes long enough to be caught half done.
  System::Call 'kernel32::GetModuleHandle(p 0) p .r6'
  System::Call 'user32::CreateWindowEx(i 0, t "STATIC", t "", i 0x5000000E, \
    i 0, i 0, i $CanvasW, i $CanvasH, p $0, p 0, p $6, p 0) p .r7'
  StrCpy $Canvas $7

  ; The flash is the static erasing ITSELF. A bitmap static asks its parent for a brush
  ; through WM_CTLCOLORSTATIC, fills its whole client area with it, and only then draws the
  ; bitmap: that fill is the dialog face colour. Clipping the parent cannot help, because the
  ; child is the one painting. A hollow brush removes the fill entirely, so the previous frame
  ; stays on screen until the new one lands on top of it.
  SetCtlColors $Canvas 0xFFFFFF transparent

  ; set once and never again: from here on the pixels change, not the handle
  SendMessage $Canvas ${STM_SETIMAGE} ${IMAGE_BITMAP} $DibShown

  System::Call 'gdiplus::GdipCreateBitmapFromFile(w "$ArtDir\o\back.png", *p .r1) i .r2'
  ${If} $2 = 0
    StrCpy $BackImg $1
  ${EndIf}
  System::Call 'gdiplus::GdipCreateBitmapFromFile(w "$ArtDir\o\box-on.png", *p .r1) i .r2'
  ${If} $2 = 0
    StrCpy $BoxOn $1
  ${EndIf}
  System::Call 'gdiplus::GdipCreateBitmapFromFile(w "$ArtDir\o\box-off.png", *p .r1) i .r2'
  ${If} $2 = 0
    StrCpy $BoxOff $1
  ${EndIf}

  StrCpy $Frame 0
  StrCpy $OverName ""
  StrCpy $OverImg 0
  StrCpy $Hot ""
  StrCpy $Leaving 0
  ; assume the button may still be down from the click that brought us here, so the next page
  ; needs a real release before it accepts anything
  StrCpy $WasDown 1
FunctionEnd

Function WindCanvasFree
  ${If} $BackImg <> 0
    System::Call 'gdiplus::GdipDisposeImage(p $BackImg)'
    StrCpy $BackImg 0
  ${EndIf}
  ${If} $BoxOn <> 0
    System::Call 'gdiplus::GdipDisposeImage(p $BoxOn)'
    StrCpy $BoxOn 0
  ${EndIf}
  ${If} $BoxOff <> 0
    System::Call 'gdiplus::GdipDisposeImage(p $BoxOff)'
    StrCpy $BoxOff 0
  ${EndIf}
  ${If} $OverImg <> 0
    System::Call 'gdiplus::GdipDisposeImage(p $OverImg)'
    StrCpy $OverImg 0
  ${EndIf}
  ${If} $MemDC <> 0
    System::Call 'gdi32::DeleteDC(p $MemDC)'
    StrCpy $MemDC 0
  ${EndIf}
  ${If} $DibBmp <> 0
    System::Call 'gdi32::DeleteObject(p $DibBmp)'
    StrCpy $DibBmp 0
  ${EndIf}
  ${If} $DibShown <> 0
    System::Call 'gdi32::DeleteObject(p $DibShown)'
    StrCpy $DibShown 0
  ${EndIf}
FunctionEnd

!macro STAMP_BOX RECT STATE
  !insertmacro OAT ${RECT}
  ${If} ${STATE} = 1
    System::Call 'gdiplus::GdipDrawImageRectI(p $3, p $BoxOn, i $R0, i $R1, i $R2, i $R3) i'
  ${Else}
    System::Call 'gdiplus::GdipDrawImageRectI(p $3, p $BoxOff, i $R0, i $R1, i $R2, i $R3) i'
  ${EndIf}
!macroend

; ---- drawing -----------------------------------------------------------------
Function WindDraw
  ; draw into the buffer the static is not showing
  System::Call 'gdi32::SelectObject(p $MemDC, p $DibBmp)'

  ; 1. the frame of the loop
  IntOp $0 $Frame + 1000        ; cheap zero padding: 1000+n -> "1042" -> "042"
  StrCpy $0 $0 3 1
  System::Call 'gdiplus::GdipCreateBitmapFromFile(w "$ArtDir\v\$0.jpg", *p .r1) i .r2'
  System::Call 'gdiplus::GdipCreateFromHDC(p $MemDC, *p .r3) i'
  ${If} $2 = 0
    System::Call 'gdiplus::GdipDrawImageRectI(p $3, p $1, i 0, i 0, i $CanvasW, i $CanvasH) i'
    System::Call 'gdiplus::GdipDisposeImage(p $1)'
  ${EndIf}

  ; 2. the shade and the caption scrim, which never change
  ${If} $BackImg <> 0
    System::Call 'gdiplus::GdipDrawImageRectI(p $3, p $BackImg, i 0, i 0, i $CanvasW, i $CanvasH) i'
  ${EndIf}

  ; 3. this screen's type, in its current hover state, alpha and all
  ${If} $OverImg <> 0
    System::Call 'gdiplus::GdipDrawImageRectI(p $3, p $OverImg, i 0, i 0, i $CanvasW, i $CanvasH) i'
  ${EndIf}
  ; the options, in whatever state they are actually in
  ${If} $Screen = 1
    !insertmacro STAMP_BOX SETUP_BOX_AUTOSTART $WantAutostart
  ${ElseIf} $Screen = 3
    !insertmacro STAMP_BOX DONE_BOX_RUN $RunAfter
    !insertmacro STAMP_BOX DONE_BOX_DESK $WantDesktop
  ${EndIf}
  System::Call 'gdiplus::GdipDeleteGraphics(p $3)'

  ; 4. the one thing a bitmap cannot know: where it is going. The path is fixed, but it is
  ;    still a runtime string, and baking a drive letter into art would be a lie on a machine
  ;    whose Program Files is not on C.
  ${If} $Screen = 1
    !insertmacro OAT SETUP_PATH
    System::Call 'gdi32::SetBkMode(p $MemDC, i ${TRANSPARENT})'
    System::Call 'gdi32::SetTextColor(p $MemDC, i 0xE6E2DC)'
    System::Call 'gdi32::SelectObject(p $MemDC, p $Font)'
    IntOp $8 $R0 + $R2
    IntOp $9 $R1 + $R3
    System::Call '*(i $R0, i $R1, i $8, i $9) p .r4'
    ; single line, vertically centred, ellipsis when the path is too long
    System::Call 'user32::DrawTextW(p $MemDC, w "$INSTDIR", i -1, p $4, i 0x00008024)'
    System::Free $4
  ${EndIf}

  ; Finish the frame, hand the DC back its own bitmap, then copy the pixels into the bitmap
  ; the static holds and repaint without erasing. No STM_SETIMAGE, so nothing paints the
  ; background between frames.
  System::Call 'gdi32::GdiFlush()'
  System::Call 'gdi32::SelectObject(p $MemDC, p $OldBmp)'
  System::Call 'kernel32::RtlMoveMemory(p $ShownBits, p $DibBits, i $DibBytes)'
  System::Call 'user32::InvalidateRect(p $Canvas, p 0, i 0)'
  System::Call 'user32::UpdateWindow(p $Canvas)'
FunctionEnd

; Load the overlay named by $1 (for example "welcome" or "setup-hot-next"), but only when it
; is not the one already loaded.
Function WindOverlay
  Pop $1
  ${If} $1 == $OverName
    Return
  ${EndIf}
  StrCpy $OverName $1
  ${If} $OverImg <> 0
    System::Call 'gdiplus::GdipDisposeImage(p $OverImg)'
    StrCpy $OverImg 0
  ${EndIf}
  System::Call 'gdiplus::GdipCreateBitmapFromFile(w "$ArtDir\o\$1.png", *p .r2) i .r3'
  ${If} $3 = 0
    StrCpy $OverImg $2
  ${EndIf}
FunctionEnd

; ---- input -------------------------------------------------------------------
; No control is clickable, because there are no controls: the same tick that draws the picture
; asks where the pointer is and whether its button just fell.
Function WindInput
  System::Call '*(i,i) p.s'
  Pop $9
  System::Call 'user32::GetCursorPos(p $9)'
  System::Call 'user32::ScreenToClient(p $Canvas, p $9)'
  System::Call '*$9(i.r6, i.r7)'
  System::Free $9

  StrCpy $Hot ""
  ; the window buttons sit in the same place on every screen
  !insertmacro HITS WELCOME_CAP_X $0
  ${If} $0 = 1
    StrCpy $Hot "close"
  ${Else}
    !insertmacro HITS WELCOME_CAP_MIN $0
    ${If} $0 = 1
      StrCpy $Hot "min"
    ${EndIf}
  ${EndIf}

  ${If} $Hot == ""
    ${If} $Screen = 0
      !insertmacro HITS WELCOME_NEXT $0
      ${If} $0 = 1
        StrCpy $Hot "next"
      ${EndIf}
    ${ElseIf} $Screen = 1
      !insertmacro HITS SETUP_NEXT $0
      ${If} $0 = 1
        StrCpy $Hot "next"
      ${Else}
        !insertmacro HITS SETUP_BACK $0
        ${If} $0 = 1
          StrCpy $Hot "back"
        ${EndIf}
      ${EndIf}
    ${ElseIf} $Screen = 3
      !insertmacro HITS DONE_NEXT $0
      ${If} $0 = 1
        StrCpy $Hot "next"
      ${EndIf}
    ${EndIf}
  ${EndIf}

  ; A click is an edge. The low bit of GetAsyncKeyState means "pressed since I last asked",
  ; which catches a click that began and ended between two ticks; the high bit is the button's
  ; state right now. Only our own window's clicks count, or a click in another app would press
  ; whatever we are hovering.
  System::Call 'user32::GetForegroundWindow() p .r2'
  System::Call 'user32::GetAsyncKeyState(i 1) i .r1'
  IntOp $3 $1 & 0x8000
  IntOp $4 $1 & 1
  ${If} $2 = $HWNDPARENT
    ${If} $4 <> 0
    ${OrIf} $3 <> 0
      ${AndIf} $WasDown = 0
        Call WindClick
    ${EndIf}
  ${EndIf}
  ${If} $3 = 0
    StrCpy $WasDown 0
  ${Else}
    StrCpy $WasDown 1
  ${EndIf}
FunctionEnd

; Post, never send. SendMessage would run the whole page change inside this callback, and NSIS
; would then destroy the dialog whose WM_TIMER we are standing in: CoreMessaging takes that
; badly (0xC00001AD).
!macro POST_CMD ID
  StrCpy $Leaving 1
  System::Call 'user32::PostMessageW(p $HWNDPARENT, i ${WM_COMMAND}, p ${ID}, p 0)'
!macroend

!macro TOGGLE_ROW RECT STATE
  !insertmacro HITS ${RECT} $0
  ${If} $0 = 1
    ${If} ${STATE} = 1
      StrCpy ${STATE} 0
    ${Else}
      StrCpy ${STATE} 1
    ${EndIf}
    Return
  ${EndIf}
!macroend

Function WindClick
  ${If} $Hot == "close"
    !insertmacro POST_CMD 2
    Return
  ${EndIf}
  ${If} $Hot == "min"
    ShowWindow $HWNDPARENT 6      ; SW_MINIMIZE
    Return
  ${EndIf}
  ${If} $Hot == "next"
    !insertmacro POST_CMD 1
    Return
  ${EndIf}
  ${If} $Hot == "back"
    !insertmacro POST_CMD 3
    Return
  ${EndIf}
  ${If} $Screen = 1
    !insertmacro TOGGLE_ROW SETUP_OPT_AUTOSTART $WantAutostart
  ${ElseIf} $Screen = 3
    !insertmacro TOGGLE_ROW DONE_OPT_RUN $RunAfter
    !insertmacro TOGGLE_ROW DONE_OPT_DESK $WantDesktop
  ${EndIf}

  ; nothing hit, and the pointer is up in the caption: drag the window. Windows runs its own
  ; loop inside this call, so the picture holds still until you let go, which is the one place
  ; that is fine.
  IntOp $8 38 * $Dpi
  IntOp $8 $8 / 96
  ${If} $7 < $8
    System::Call 'user32::ReleaseCapture()'
    SendMessage $HWNDPARENT ${WM_NCLBUTTONDOWN} ${HTCAPTION} 0
  ${EndIf}
FunctionEnd

; ---- the tick ----------------------------------------------------------------
Function WindTick
  ${If} $Leaving = 1
    Return
  ${EndIf}
  IntOp $Frame $Frame + 1
  ${If} $Frame >= ${FRAMES}
    StrCpy $Frame 0
  ${EndIf}
  Call WindInput
  Call WindPickOverlay
  Call WindDraw
FunctionEnd

Function WindPickOverlay
  ${If} $Screen = 0
    StrCpy $1 "welcome"
  ${ElseIf} $Screen = 1
    StrCpy $1 "setup"
  ${ElseIf} $Screen = 2
    StrCpy $1 "copy"
  ${Else}
    StrCpy $1 "done"
  ${EndIf}
  ${If} $Hot != ""
    StrCpy $1 "$1-hot-$Hot"
  ${EndIf}
  Push $1
  Call WindOverlay
FunctionEnd
