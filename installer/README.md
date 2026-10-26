# Setup

Wind's installer is NSIS with a custom-drawn UI: a looping video with the screens composited over
it. NSIS cannot play video, so every tick it decodes a JPEG frame through GDI+, alpha-blends the
screen's overlay on top and copies the result into one static control; the same tick hit-tests the
pointer. There are no native buttons, so setup also carries an accessible plain mode built from stock
pages (see [Accessible setup](#accessible-setup)).

Wind installs per-machine and elevated, and there is no install-location screen: UIAccess is granted
only to a signed binary in a secure location, so an install elsewhere could not get it, and Wind refuses to start without it.
The elevation rules (HKLM autostart, launching through `explorer.exe`, stopping Wind through
`Local\Wind_QuitRequest`) are in
[docs/architecture/11](../docs/architecture/11-build-test-release.md#the-installer).

## Files

| File | What it is |
|---|---|
| `wind.nsi` | Entry point: metadata, page order, install and uninstall sections |
| `app.nsh` | Wind-specific logic: quitting a running Wind, WebView2, autostart, launching |
| `screens.nsh` | The five screens and what each click does |
| `plain.nsh` | The accessible plain mode: stock MUI pages for screen readers and the keyboard |
| `kit.nsh` | The frameless window: size, DPI, GDI+, unpacking |
| `video.nsh` | The player: decode, composite, hover, clicks, dragging |
| `over.html` | The foreground: type, buttons, caption. The only place copy lives |
| `make-over.mjs` | Renders `over.html` into overlays and `over.nsh` rectangles |
| `make-loop.mjs` | Turns a source clip into a looping frame sequence |
| `over.nsh` | Generated: control rectangles in 640x480 units |
| `media/<size>/` | Generated: `v/` frames, `o/` overlays, `o/back.png` (shade and caption scrim under every screen). Committed, because CI cannot regenerate it |
| `local-sign.ps1` | Per-PC signing of the UIAccess build |
| `user-localappdata.ps1` | Finds the signed-in user's `%LOCALAPPDATA%` from the elevated setup and uninstaller |
| `MicrosoftEdgeWebview2Setup.exe` | Microsoft's Evergreen WebView2 bootstrapper |

## Build

```
build.bat installer
```

Needs NSIS (`winget install NSIS.NSIS`). It packs the binaries from `build\` (`build.bat`,
`build.bat config`) and the UI from `ui\dist`, so build those first. The target compiles the script and runs
`tools\installer_check.ps1`, which checks that every packed file exists, that every rectangle the
screens read was generated, and that a silent install and uninstall round-trip (the round trip
needs an elevated shell). For a release artifact use `tools\release.ps1`, which builds the payload,
signs it when a certificate is configured, and packs the installer.

## Local signing

Without a code-signing certificate, `release.ps1` also builds the UIAccess variant as `build\WindUA.exe`,
which makes `wind.nsi` define `LOCAL_SIGN`. Setup then runs `local-sign.ps1`:

- A fresh `CN=Wind Local Signing` certificate is trusted in LocalMachine Root and TrustedPublisher,
  signs the executables, and has its private key deleted at once, so nothing can sign with that root
  again. Older Wind roots are retired on every install; the uninstaller removes them
  (`local-sign.ps1 -Remove`).
- Setup installs the ordinary build first and signs the UIAccess build in the admin-only
  `$INSTDIR\.stage` (removed afterwards; never the user-writable `$PLUGINSDIR`), copying it
  over only after its signature verifies. An unsigned UIAccess `Wind.exe` does not start at all, so
  any failure leaves the ordinary build. Wind refuses to start without UIAccess (issue #460), so
  setup then shows an error and does not launch it.
- A release signed with a real certificate has no `WindUA.exe` and skips this. Any `Wind.exe` build
  deletes `WindUA.exe` (`build.bat :core_exe`) unless `release.ps1` just made both, so `build.bat
  installer` never packs a stale one.

## Licence screen

Two of the five screens are the licence page, before and after its box is ticked
(`windLicenceCreate`/`windLicenceLeave` in `screens.nsh`). Install stays disabled until the box is
ticked. "Read the full licence" copies `LICENSE.txt` to a fresh folder in the user's temp directory
and opens it through `explorer.exe`, so the viewer is not elevated (an elevated NSIS locks
`$PLUGINSDIR` to Administrators). A silent install skips the page; `LICENSE.txt` is installed next to
`Wind.exe` either way.

## Accessible setup

The picture is pixels: a screen reader finds nothing to name on it and the keyboard has nothing to
focus (issue #513). So `wind.nsi` lists two page sets and `.onInit` (`WIND_PICK_PLAIN` in `kit.nsh`)
picks one per run. The plain set is stock NSIS: MUI's welcome page, MUI's licence page with the full
`LICENSE` text and a real "I accept" checkbox, an nsDialogs page with the install folder as a label
and a "Start Wind when I sign in" checkbox, the stock install page with its status line and log
shown, and MUI's finish page with "Open Wind now" (launched through `explorer.exe`, like the picture)
and "Create desktop shortcut". NVDA, JAWS and Narrator read these, and Tab, Space and Enter work.

Setup runs plain when any of these holds:

- `/ACCESSIBLE` is on the command line (any case): `Wind-Setup-x64-<ver>.exe /ACCESSIBLE`.
- A screen reader has set the system flag (`SPI_GETSCREENREADER`).
- NVDA is running (its `wxWindowClassNR` / `NVDA` window exists).
- High contrast is on (the art ignores the user's colours).

A silent install (`/S`) never runs plain mode and behaves exactly as before. Plain mode skips
everything the picture needs: the frameless window, the media unpack, GDI+ and the timer, the hidden
log and the autoclose. Each plain MUI page has `windPlainOnly` as its PRE function, and each picture
creator aborts when `$Plain` is 1, so NSIS walks past the other set in both directions.

The picture itself answers one key: Space ticks the licence box, and pressing Enter on the licence
screen before ticking it shows a message box saying so and pointing at `/ACCESSIBLE`, rather than
doing nothing.

## Editing the overlay

Edit `over.html`, then regenerate both overlay sets straight into `media/<size>/o`:

```
node installer/make-over.mjs 1440
node installer/make-over.mjs 960
```

Renaming a `data-a` attribute renames its rectangle in `over.nsh`. That compiles and then hit-tests
against nothing, which `installer_check.ps1` catches, so run `build.bat installer` after such an
edit. Each screen is two layers: `back.png` (shared) and a per-screen overlay with type and
controls, so the shared gradient is stored once.

## Replacing the clip

The loop in `media/<size>/v` is still Prism's placeholder clip. Swap it with `make-loop.mjs` once a
Wind clip exists:

```
node installer/make-loop.mjs "C:\path\to\clip.mp4" --len 16 --fps 24
```

The script sizes the loop to the frames the source yields and crossfades the tail into the head; it
prints the frame count and the wrap error. Put the frame count in `FRAMES` in `video.nsh` and set
`TICK` to 1000 / frame rate. Needs `ffmpeg` and ImageMagick (`magick`) on PATH. The footage ships at
one size for every display; the overlays render at the display's resolution (960 or 1440 set).
