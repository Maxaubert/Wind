# Setup

Setup is a video with the UI composited over it. NSIS cannot play video, so it plays one
itself: every tick it decodes a JPEG through GDI+, alpha-blends the screen's overlay on top,
and copies the result into the bitmap a single static control shows. There are no buttons;
the same tick hit-tests the pointer.

The approach is borrowed from Prism's installer. What differs here is that Wind installs
per-machine and elevated, which the sections have to be careful about, and that there is no
"where it goes" screen: UIAccess is only granted to a signed binary in a secure location, so
an install to `D:\Apps\Wind` would silently disable the features the per-machine install
exists to enable. The path is shown and the reason is given instead of offering a chooser
whose wrong answers are quiet.

## The pieces

| file | what it is |
|---|---|
| `wind.nsi` | the entry point: metadata, page order, the install and uninstall sections |
| `app.nsh` | the Wind-specific half: quitting a running Wind, WebView2, autostart, launching |
| `over.html` | the foreground: type, buttons, the caption. The only place copy lives. |
| `make-over.mjs` | renders `over.html` into alpha overlays + `over.nsh` rectangles |
| `make-loop.mjs` | turns a source clip into the frame sequence, and makes it loop |
| `kit.nsh` | the frameless window: size, DPI, GDI+, unpacking |
| `video.nsh` | the player: decode, composite, hover, clicks, dragging |
| `screens.nsh` | the four screens, and what each click means |
| `over.nsh` | generated: control rectangles in 640x480 units |
| `media/<size>/` | generated: `v/` frames, `o/` overlays. Not hand-edited. |
| `media/<size>/o/back.png` | generated: the shade, the rim and the caption scrim, drawn under every screen |
| `MicrosoftEdgeWebview2Setup.exe` | Microsoft's ~1.7 MB Evergreen bootstrapper stub |

## Building it

```
build.bat installer
```

Needs NSIS (`winget install NSIS.NSIS`). That target compiles the script and then runs
`tools\installer_check.ps1`, which verifies things a compile cannot: that every rectangle the
pages read was actually generated, and that a silent install and uninstall round-trip. The
round-trip half needs an elevated shell and skips itself without one.

For a release artifact, use `tools\release.ps1` instead: it builds the payload, signs it when
a certificate is configured, and packs the installer.

## Changing the words or the layout

Edit `over.html`, then regenerate both overlay sets:

```
node installer/make-over.mjs 1440
node installer/make-over.mjs 960
```

They write straight into `media/<size>/o`, which is what the installer packs. Do not stage
them anywhere else: an intermediate folder is a thing to forget.

Renaming a `data-a` attribute renames its rectangle in `over.nsh`. That compiles fine and then
hit-tests against nothing, which is exactly what the rectangle check in `installer_check.ps1`
is there to catch, so run `build.bat installer` after any such edit.

## Changing the clip

The footage currently shipping is a blue flow-line abstract, cut from the first 15 seconds of
the source clip: it drifts teal after that, and blue is the family Wind's indigo accent lives
in. It was built with

```
node installer/make-loop.mjs "wave-abstract-background.1920x1080.mp4" --start 0 --len 15 --fps 24 --fade 36
```

which yielded 324 frames, so `FRAMES` in `video.nsh` is 324 and `TICK` is 42. The wrap measures
RMSE 0.0369 against a natural frame-to-frame range of 0.0099 to 0.0319, so the seam is a little
above the clip's own fastest moment and reads as motion rather than a cut.

To replace it:

```
node installer/make-loop.mjs "C:\path\to\clip.mp4" --len 16 --fps 24
```

It reports how many frames it produced; put that number in `FRAMES` in `video.nsh`, and set
`TICK` to 1000 / the clip's frame rate. The script reads how many frames the source actually
yielded and sizes the loop to fit, and crossfades the tail into the head so the wrap is
smaller than an ordinary frame step. It prints both numbers so you can check.

Needs `ffmpeg` and ImageMagick (`magick`) on PATH.

## Two overlay layers

Each screen is drawn as **two** overlays, not one: `back.png` carries the shade, the lens rim
and the caption scrim, and the per-screen overlay carries only type and controls. They are
identical layers on every screen and every hover state, so baking them together would store
the same full-frame gradient nineteen times: measured, that was 28.3 MB of media against
13.3 MB for the split. The cost is one extra `GdipDrawImageRectI` per tick.

## What it costs

Roughly 7.6 MB of frames at 800x600, plus about 4 MB of overlays across both DPI sets and the
1.7 MB WebView2 stub, which packs down to a 12.3 MB installer. Both overlay sets are packed into the
installer and only the matching one is unpacked at runtime.

The footage ships at one size for every display because it is defocused motion and an upscale
is invisible on it; the type is a separate overlay and renders at the display's own
resolution.
