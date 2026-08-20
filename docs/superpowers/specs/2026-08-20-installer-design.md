# Wind installer design

Status: proposed
Issue: TBD (opened alongside this spec)
Related: Prism's `build/installer/` (the presentation layer this borrows)

## Goal

Ship `Wind-Setup-x64-<version>.exe` on GitHub Releases: a per-machine installer that puts Wind in
`C:\Program Files\Wind`, looks like Prism's setup (a video loop with type composited over it, no
visible controls), and leaves a clean uninstall behind.

Wind has no packaging today. `tools\uiaccess_setup.ps1` is a dev deploy: it mints a self-signed
cert, trusts it machine-wide, builds, signs and copies. Correct for this box, wrong for a
stranger's.

## Decisions taken up front

| Question | Decision | Why |
|---|---|---|
| Install scope | Per-machine, `C:\Program Files\Wind`, elevated | UIAccess requires a "secure location"; a per-user install can never enable it |
| Audience | Public GitHub Releases | Drives signing, SmartScreen, WebView2 bootstrap, clean uninstall |
| Signing | Designed for, switched off until a cert exists | No cert purchase; SignPath Foundation issues free OV certs to OSS projects |
| Footage | Prism's clip as a placeholder | A Wind clip arrives later; `make-loop.mjs` makes the swap one command |
| Autostart | Installer owns it (HKLM Run) | Wind is a tray app with no autostart today |
| Updates | None in v1 | The next installer upgrades in place |

## What UIAccess actually buys, and what unsigned costs

This drives the signing design, so it is stated precisely rather than assumed.

UIAccess is NOT required for keybinds in general. `WH_KEYBOARD_LL`, `WH_MOUSE_LL` and
`RegisterHotKey` work from any normal process, so zoom binds, Inspect, recenter and input
swallowing all work in an unsigned build. UIAccess buys exactly three things:

1. Binds keep working while an **elevated** window holds focus (Task Manager, regedit, an elevated
   terminal). Without it UIPI silently blocks the hook there.
2. `desktopTransform`: `MagSetInputTransform` returns ACCESS_DENIED without UIAccess, so hybrid
   keeps the desktop on render. `transform_model.cpp:122` already probes `TokenUIAccess` and
   disables the desktop pick cleanly, with no session ever created.
3. `zorderBand=16` shell coverage, which ships as `0` regardless (issue #162).

An unsigned Wind is therefore fully usable and already degrades correctly. No app change is needed
to support the unsigned path.

A `uiAccess=true` manifest on an unsigned binary is denied by AppInfo rather than fatal, but the
release script ships the `uiAccess=false` variant when unsigned anyway: shipping a manifest that
asks for a privilege it cannot have is noise in a public artifact.

## Technology

NSIS 3.x, hand-written script. Prism reaches NSIS through electron-builder; Wind is C++/MSVC so
there is no builder to hook, and the electron-builder glue (`customWelcomePage`, `customInstall`,
`$isForceCurrentInstall`, `${BUILD_RESOURCES_DIR}`) is replaced by plain `Page custom` declarations
and local defines. The presentation layer, the part worth borrowing, ports nearly verbatim.

Rejected: **Inno Setup**. Friendlier scripting, but no cheap way to own the window and composite
frames; porting Prism's approach means fighting its page model.

Rejected: **a bespoke C++ installer**. Wind already has a D3D pipeline and could play real video,
but the value here is the look, which NSIS already delivers via proven code, and we would rewrite
file copy, registry, ARP and uninstall from scratch.

New build dependency: NSIS, discovered by `build.bat` the way `vswhere` is, installable with
`winget install NSIS.NSIS`.

## Layout

```
installer/
  wind.nsi          entry: metadata, page order, sections, uninstaller
  kit.nsh           frameless window, DPI, GDI+, unpack        (ported from Prism)
  video.nsh         decode + alpha composite + hit-test        (ported, near verbatim)
  over.nsh          generated: control rectangles in 640x480 units
  over.html         Wind's copy and layout. The only place copy lives.
  make-over.mjs     over.html -> alpha overlays + over.nsh     (Playwright, not Electron)
  make-loop.mjs     a clip -> seamless JPEG sequence           (ported from make-loop.cjs)
  app.nsh           Wind-specific: quit running instances, WebView2, autostart, ARP
  media/            generated. 800/v frames, 960/o + 1440/o overlays. Not hand-edited.
tools/
  release.ps1          build -> optional sign -> makensis -> Wind-Setup-x64-<ver>.exe
  installer_check.ps1  build gate: compile, assert media + rects, silent-install smoke
```

## How the picture works (inherited from Prism)

NSIS cannot play video, so setup plays one itself. `kit.nsh` strips the wizard frame, header,
hairline and three buttons, sizes a frameless rounded window to the art and centres it on the work
area. `video.nsh` runs a 1 ms-resolution timer that, each tick, decodes one JPEG through GDI+,
alpha-blends the screen's overlay PNG on top and blits the result into a single static control,
then hit-tests the pointer by hand. There is not one real control on any screen.

Alpha is solved for, not captured: `make-over.mjs` renders each screen twice, once on black and
once on white, and for a pixel of colour C at coverage a solves `a = 1 - (B - A)`, `C = A / a`.
Screenshotting a transparent window on Windows does not reliably return antialiased type, soft
shadows or the glow under a button.

Anything whose value changes at runtime (the progress fill, a checkbox state) is left OUT of the
art and painted by NSIS into a rectangle that `over.nsh` names.

## The four screens

Prism's rhythm, Wind's content, and one substantive change.

Prism's "where it goes" screen is **replaced**, not ported. A browsable install path is actively
wrong for Wind: UIAccess requires a secure location, so letting someone install to `D:\Apps\Wind`
would permanently and silently disable the features the per-machine install exists to enable.
Screen 2 shows the fixed location, says why in a line of small type, and carries the one choice
that matters.

1. **Welcome** - "Install Wind", one line of subtitle, Install.
2. **Setup** - `C:\Program Files\Wind` shown and not editable, with the reason. Checkbox: "Start
   Wind when I sign in" (on). Back / Install.
3. **Installing** - one frame plus the progress bar: theme off, `PBS_SMOOTH`, recoloured to Wind's
   palette and sat on the drawn trough. Same constraint as Prism, the section runs on the script
   thread so nothing can call back into script while files are written.
4. **Done** - "Wind installed". Checkboxes: "Open Wind now" (on), "Create desktop shortcut" (off).
   Finish.

A caption strip carries the Wind mark, a minimise and a close, and is dragged by hit-testing the
top strip, since there is no title bar.

## Elevation, and the two traps it sets

`RequestExecutionLevel admin`. Prism is deliberately per-user and never elevates, so neither of
these exists there and neither can be copied from it.

**Launching Wind at the end must de-elevate.** A plain `Exec` from an elevated installer hands Wind
an admin token. `ResolveIniPath()` would then resolve `%LOCALAPPDATA%` to the *administrator's*
profile, so the ini, profiles and logs land where the user will never see them, and the tray app
runs permanently elevated for no reason. The Done screen uses `ShellExecAsUser` so Wind starts as
the signed-in user. This bites whenever a standard user elevates with an admin account, which is
the normal field case even though it is not the case on this box.

**Autostart goes in HKLM, not HKCU.** An `HKCU\...\Run` write from an elevated installer lands in
whichever hive the elevated token owns, which is the wrong user in exactly the same case.
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run` is consistent with a per-machine install,
appears in Task Manager's Startup tab where users expect to manage it, and is removed by the
uninstaller. Wind's single-instance mutex is `Local\`-scoped, so one instance per session is
preserved.

## Stopping the running instance

Every upgrade runs over a live tray app holding its own exe open. Wind already has the right
channel: `main.cpp:2199` creates an auto-reset named event `Local\Wind_QuitRequest`, and
`main.cpp:1917` shows the existing consumer. Setting it makes Wind exit **cleanly**, which restores
the OS cursor, releases any `ClipCursor`, releases the Magnification runtime, and restores the
user's native-Magnifier registry backup if the magnify model ever modified it. Terminating the
process skips all of that and can strand a hidden cursor or a 1px cursor clip.

`app.nsh` therefore opens the event, sets it, waits up to 5 s for the process to go, and only then
falls back to a kill. `WindConfig.exe` gets `WM_CLOSE` then a kill; it holds no OS state.

`Local\` is session-scoped and UAC elevation stays within the session, so this works from the
elevated installer.

## WebView2

`WindConfig.exe` fails to a blank window without the Evergreen runtime. Detect it by reading `pv`
under `HKLM\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}`
and the HKCU equivalent, treating an empty or `0.0.0.0` value as absent. Missing means run
Microsoft's ~2 MB `MicrosoftEdgeWebview2Setup.exe` bootstrapper silently inside the install section.
Present on effectively every Windows 11 machine, so this is a rare branch, not a bundled runtime.

## Signing

`tools\release.ps1` signs `Wind.exe`, `WindConfig.exe` and then the finished installer when a cert
is configured in the environment (`WIND_SIGN_THUMBPRINT`, or `WIND_SIGN_PFX` plus
`WIND_SIGN_PASSWORD`). No secret enters the repo.

- Cert present: build the `uiaccess` variant, sign, print the verified signer.
- Cert absent: build the normal variant and print `unsigned build: UIAccess features disabled
  (elevated-window keybinds, desktopTransform)`.

One knob, and nothing in the installer changes when the cert arrives.

Add a `LICENSE` (MIT) to the repo. SignPath Foundation requires an OSS licence, and Wind currently
has none, which is the only thing blocking a free OV certificate.

## Uninstall

An ARP entry under `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Wind` carries
DisplayName, DisplayVersion (from `version.h`), Publisher, DisplayIcon, EstimatedSize and
NoModify/NoRepair.

The uninstaller quits Wind through the same named event, then removes the install directory, the
HKLM Run entry, the shortcuts and the ARP key. It then asks one question: keep or delete
`%LOCALAPPDATA%\Wind` (ini, profiles, logs, the churny-apps list). Default keep.

Restoring the user's native-Magnifier registry needs no uninstaller code: quitting Wind cleanly
already does it, from the snapshot in `%LOCALAPPDATA%\Wind\magnifier_backup.ini`.

## Version as one source of truth

`wind.nsi` reads `src\version.h` with `!searchparse`, so `WIND_VER_MAJOR/MINOR/PATCH` drive the ARP
version, the artifact filename and setup's own VERSIONINFO. Bumping `version.h` is the whole release
ritual.

## Media

Prism's `media/800/v/*.jpg` is copied in verbatim as the placeholder loop. `make-loop.mjs` takes a
clip, produces the frame sequence and crossfades the tail into the head so the wrap is smaller than
an ordinary frame step, reporting the frame count and tick to put in `video.nsh`.

Overlays are Wind's own from the start: `over.html` carries Wind's palette and the mark from
`assets\wind-badge.svg`.

The renderer swaps Electron for **Playwright**, already a devDependency in `ui/package.json`, so the
installer adds no new npm dependency. The two-pane black/white alpha solve is unchanged.
ImageMagick is already on PATH here and is documented as a prerequisite.

Cost, following Prism's measured numbers: roughly 10 MB of frames at 800x600 plus about 3 MB of
overlays. Footage ships at one size for every display (it is defocused motion, an upscale is
invisible on it); type is a separate overlay rendered at the display's own resolution, 960 or 1440.

## Testing

Installers resist automated testing, so the split is deliberate rather than aspirational.

**Pure logic, doctested** in the existing `src/` + `tests/` pattern, no `<windows.h>`:

- `src/installer_state.h` - version comparison and install-state classification (fresh / upgrade /
  downgrade / same-version reinstall) from a found version string and ours.
- `src/webview2_probe.h` - the `pv` string parse and the absent / `0.0.0.0` rule.

**Build gate**, `tools\installer_check.ps1`, run by `build.bat installer`:

- compiles `wind.nsi` and fails on any warning,
- asserts every media file `kit.nsh` packs exists,
- asserts every rectangle name the pages read is present in the generated `over.nsh`, which is the
  failure mode a copy edit causes and a compile does not catch,
- runs `Wind-Setup.exe /S` into a scratch prefix and asserts the files, ARP key and Run entry
  appear, then runs the uninstaller silently and asserts they are gone.

**Honest limit:** `/S` exercises the section, not the drawn UI, and the drawn UI is most of the
code. A manual matrix therefore goes into `docs/VERIFICATION.md`: fresh install, upgrade over a
running instance, uninstall keeping data, uninstall deleting data, at 100% and at 225% DPI.

## Out of scope

Auto-update, an MSI or winget manifest, a per-user install mode, ARM64, and localisation. Each is
separate work that this does not foreclose.
