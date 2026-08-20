;
; Wind setup, the parts that are about Wind rather than about installing.
;
; Four things the generic script does not know: how to make a running Wind let go of its
; own exe, whether WindConfig has a browser engine to run in, where autostart lives when
; the installer is elevated, and how to hand the app back to the user who asked for it.
;

!include "LogicLib.nsh"

!define WV2_CLIENT "SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"

; Wind's own two kernel objects. Both are Local\, i.e. session-scoped, and UAC elevation
; stays inside the session, so the elevated installer and the user's Wind see the same
; objects. Names must match src\main.cpp:1911 and src\main.cpp:2199 exactly.
!define WIND_MUTEX "Local\Wind_Magnifier_SingleInstance"
!define WIND_QUIT  "Local\Wind_QuitRequest"

!define EVENT_MODIFY_STATE 0x0002
!define SYNCHRONIZE        0x00100000

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
; The wait is on Wind's single-instance mutex rather than a tasklist poll, which is both
; exact and instant: Wind holds it for its whole run and releases it on exit, and this is
; the same handshake Wind itself performs when one instance evicts another.
!macro WIND_QUIT_RUNNING
  DetailPrint "Closing Wind..."

  ; WindConfig first, so it cannot relaunch Wind while we are stopping it. Force, because
  ; a WM_CLOSE with unsaved settings opens a confirm dialog that would hang setup
  ; (src\config_ui\main.cpp:490). It holds no OS state, so there is nothing to lose.
  nsExec::Exec 'taskkill /IM WindConfig.exe /F'
  Pop $0

  System::Call 'kernel32::OpenEventW(i ${EVENT_MODIFY_STATE}, i 0, w "${WIND_QUIT}") p .r0'
  ${If} $0 <> 0
    System::Call 'kernel32::SetEvent(p $0)'
    System::Call 'kernel32::CloseHandle(p $0)'

    ; The mutex comes free the moment Wind lets go of it, which is EARLY in its teardown
    ; and not the same thing as the process being gone (rig-probed: WAIT_OBJECT_0 after
    ; 3 ms, process actually gone a few hundred ms later). So this is a fast first signal,
    ; not the finish line. WaitForSingleObject on a mutex ACQUIRES it, so it is released
    ; again straight away or the Wind we are about to launch would refuse to start
    ; (main.cpp:1926).
    System::Call 'kernel32::OpenMutexW(i ${SYNCHRONIZE}, i 0, w "${WIND_MUTEX}") p .r1'
    ${If} $1 <> 0
      System::Call 'kernel32::WaitForSingleObject(p $1, i 5000) i .r2'
      ${If} $2 == 0
      ${OrIf} $2 == 128        ; WAIT_ABANDONED: it died holding the mutex, still ours now
        System::Call 'kernel32::ReleaseMutex(p $1)'
      ${EndIf}
      System::Call 'kernel32::CloseHandle(p $1)'
    ${EndIf}

    ; Now wait for the PROCESS. This gate is the whole point of asking politely: killing
    ; Wind between "released the mutex" and "finished shutting down" would skip the cursor
    ; restore, the ClipCursor release and the Magnifier registry restore, which is exactly
    ; the damage the quit event exists to avoid. Up to 5 s in 250 ms steps, so the normal
    ; case costs a quarter of a second.
    StrCpy $3 0
    ${Do}
      nsExec::Exec 'cmd /c tasklist /FI "IMAGENAME eq Wind.exe" /NH | find /I "Wind.exe"'
      Pop $4
      ${If} $4 != 0
        ${Break}               ; find found nothing: the process is gone
      ${EndIf}
      Sleep 250
      IntOp $3 $3 + 1
    ${LoopUntil} $3 >= 20
  ${EndIf}

  ; Fallback only, for a Wind that ignored the request or never had the event open (an
  ; older build, a hung tick loop). Checked first rather than fired unconditionally, so a
  ; Wind that IS shutting down cleanly is never cut off partway through. The installer is
  ; elevated, so this succeeds even against the signed UIAccess build's integrity level.
  nsExec::Exec 'cmd /c tasklist /FI "IMAGENAME eq Wind.exe" /NH | find /I "Wind.exe"'
  Pop $4
  ${If} $4 == 0
    DetailPrint "Wind did not respond to the quit request; stopping it."
    nsExec::Exec 'taskkill /IM Wind.exe /F'
    Pop $0
  ${EndIf}
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
    ; ~1.7 MB stub that pulls the runtime itself, rather than bundling ~150 MB we would
    ; ship to every user to serve the small minority that lack it.
    File "/oname=$PLUGINSDIR\MicrosoftEdgeWebview2Setup.exe" "MicrosoftEdgeWebview2Setup.exe"
    nsExec::Exec '"$PLUGINSDIR\MicrosoftEdgeWebview2Setup.exe" /silent /install'
    Pop $0
    ${If} $0 != 0
      ; Not fatal. Wind.exe itself needs no browser engine; only Settings does, and it can
      ; be repaired later. Failing the whole install over it would be worse.
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
; Plain Exec would hand Wind the installer's ADMIN token. wind::ResolveIniPath() would
; then resolve %LOCALAPPDATA% to the administrator's profile, so magnifier.ini, the
; profiles and the logs would land where the user never finds them, and a tray magnifier
; would run elevated forever for no reason.
;
; Passing the path to explorer.exe hands the launch to the running shell, which owns the
; signed-in user's token, so Wind starts unelevated. This is done with the shell rather
; than the usual ShellExecAsUser plugin deliberately: that plugin is a third-party binary
; NSIS does not ship, and committing an unauditable DLL to a public repository is a worse
; trade than one documented line. Exec returns as soon as explorer takes the request, so
; there is no exit code to check and none is wanted.
!macro WIND_LAUNCH_DEELEVATED
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\Wind.exe"'
!macroend
