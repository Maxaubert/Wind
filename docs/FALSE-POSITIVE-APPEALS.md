# False-positive appeals: Wind 0.6.1

Prepared 2026-09-02. Everything below is ready to paste into the submission forms. The one gap
is marked **[NEED FROM JOB PC]** - the appeal cannot be filed properly without it.

## The artifact

| Field | Value |
| --- | --- |
| File | `Wind-Setup-x64-0.6.1.exe` |
| Size | 12,839,056 bytes |
| SHA256 | `041ddc39b16dbbba4b585aca603e58b9afc562a5ac8d9a992661ec42cddf639b` |
| Download URL | https://github.com/Maxaubert/Wind/releases/download/v0.6.1/Wind-Setup-x64-0.6.1.exe |
| Source | proprietary, not public (was public + MIT up to v0.6.1) |
| Publisher | Max Aubert |
| Signature | none (unsigned) |

Payload binaries inside the installer, in case a form wants the inner file rather than the setup:

| File | SHA256 |
| --- | --- |
| `Wind.exe` | `6367829396ccc86a56df5f56ba38bcbf04d11385180114a153aee4079b47cfa7` |
| `WindConfig.exe` | `d52408f018586f9b1e89cb106341394e0b8d955b4b727adb3b36c3abecd35c39` |
| `Uninstall.exe` | `25d4193722f8341871443e3a104ae4b1e2b20e0a4844a730d9d04af65052d3bc` |

Verified on 2026-09-02 against Microsoft Defender with cloud protection enabled
(`MpCmdRun -Scan -ScanType 3`): the installer and all three payload binaries return
**no threats**. So Defender itself is not currently the thing blocking the download.

## Submission 1 - Microsoft Defender (WDSI)

**URL:** https://www.microsoft.com/en-us/wdsi/filesubmission

Sign in with a Microsoft account, then:

- Submission type: **Software developer**
- Product: **Microsoft Defender Antivirus**
- What do you believe this file is: **Incorrectly detected as malware/malicious**
- Company name: `Max Aubert`
- Detection name: **[NEED FROM JOB PC]** - the exact detection string the work machine showed
  (for example `Trojan:Win32/Wacatac.B!ml`). This field is mandatory and the form cannot be
  submitted without it.
- File: upload `Wind-Setup-x64-0.6.1.exe`

Paste into **Additional information**:

> Wind is a fullscreen screen magnifier for Windows, a lightweight replacement for the built-in
> Magnify.exe. Downloads are published at https://github.com/Maxaubert/Wind-releases. The
> installer is built entirely by GitHub Actions on a clean windows-latest runner from a tagged
> commit, with no manual step and no packer beyond standard NSIS LZMA compression.
>
> The binary uses several APIs that resemble a malicious profile in aggregate, but each is
> required for a magnifier. Source references are given so they can be checked against a
> decompilation:
>
> - WH_KEYBOARD_LL and WH_MOUSE_LL hooks (src/input_router.cpp): used only to read and swallow
>   the user's own configured zoom shortcuts so they do not double-fire into the focused
>   application. No keystroke is recorded, stored, or transmitted. IsForbiddenBindVk in
>   src/config.cpp explicitly refuses to bind mouse clicks, Backspace and the Windows key.
> - Raw Input (WM_INPUT): used to keep tracking mouse movement in games that hide or lock the
>   cursor, which is the product's main feature.
> - DXGI Desktop Duplication and Direct3D 11 (src/render_engine.cpp): captures the screen in
>   order to magnify it and draw it back to a fullscreen overlay. Nothing is written to disk or
>   sent anywhere; there is no network code in the application at all.
> - SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) on the overlay window: this is not
>   anti-analysis. Desktop Duplication would otherwise capture the magnifier's own presented
>   frame and feed it back into the capture, producing an infinite feedback loop. This is
>   documented in the project's CLAUDE.md as the top rendering pitfall.
> - SendInput: used only to route a click to the point under the drawn cursor while zoomed, and
>   for the Ctrl+Alt+wheel shortcut when driving the native Windows Magnifier.
> - HKLM\Software\Microsoft\Windows\CurrentVersion\Run: the optional "start when I sign in"
>   checkbox offered during setup. HKLM rather than HKCU because the installer is elevated and
>   an elevated process's HKCU is the wrong user's hive.
>
> The application makes no network connections, contains no update or telemetry code, and writes
> only to %LOCALAPPDATA%\Wind (settings, profiles, logs).
>
> The installer is NSIS. It briefly extracts Microsoft's own official WebView2 bootstrapper
> (MicrosoftEdgeWebview2Setup.exe, shipped by Microsoft) to $PLUGINSDIR and runs it with
> /silent /install only when the WebView2 runtime is absent, because the settings window is a
> WebView2 host. This is the documented Microsoft deployment method for WebView2.
>
> The build is unsigned because no code signing certificate has been obtained yet. Please
> re-evaluate; I believe this is a machine-learning false positive driven by the combination of
> input hooks plus screen capture plus an unsigned low-reputation binary.

## Submission 2 - SmartScreen / browser reputation

There is no standalone developer form for this. The report has to be made **from the block
itself**, on the machine that saw it:

- **Microsoft Edge:** on the download warning, choose **More information** (or the `...` next to
  the blocked download) then **Report this file as safe**. That opens the SmartScreen feedback
  page with the file's identity already attached, which is why filing it from the block is worth
  far more than filing it cold.
- **Chrome:** on the blocked download, expand it and use **Report as safe** / **Send feedback**
  if the enterprise policy leaves that available. If the entry is fully greyed out there may be
  no report path at all, which is itself the answer: the block is policy, not a verdict.

SmartScreen reputation is not something an appeal really resolves. It resolves when the file is
signed by a certificate with publisher history, or when enough people download it without
incident.

## Submission 3 - Google Safe Browsing

**URL:** https://safebrowsing.google.com/safebrowsing/report_error/

Enter the download URL:

```
https://github.com/Maxaubert/Wind/releases/download/v0.6.1/Wind-Setup-x64-0.6.1.exe
```

Be aware of the limit here: the proper owner channel for a Safe Browsing download verdict is
Search Console's Security Issues report, and that requires verified ownership of the hosting
domain. The file is hosted on github.com, which cannot be verified by us. So this generic form is
the only available route and expectations should be low.

Also worth distinguishing: if Chrome said the file **"isn't commonly downloaded"**, that is not a
Safe Browsing malware verdict and there is nothing to appeal - it is pure reputation and it clears
by itself with signing or download volume. If Chrome said the file **"contains malware"** or
**"is dangerous"**, that is a real Safe Browsing verdict and the form above is the right route.

## What is still needed

The exact wording of the block on the work machine, and the detection name if one was shown.
That single string decides which of the three submissions above is the applicable one:

- A named detection (`Trojan:Win32/...`) means Defender or a third-party AV. Submission 1.
- "could harm your device" / "blocked because it is not commonly downloaded" means SmartScreen.
  Submission 2.
- "is dangerous" / "contains malware" in Chrome means Safe Browsing. Submission 3.
- No message, just a greyed-out entry, means enterprise download policy, and none of the three
  applies - only a signature would change it.

## Timing note

This packet was written while the repository was still public under MIT. The single strongest
argument in a false-positive submission is "the reviewer can rebuild this artifact from public
source in a few minutes", and that argument does not survive the repo going private. If these
are filed after the source is closed, expect a weaker result and lean on the signature instead:
a signed binary from a verified publisher is what actually resolves this class of block.
