# Wobble capture, 2026-08-21 (~11:45 local)

Live capture of the degenerate cursor state Max reported: cursor not locked to screen centre,
wobbles with inertia, trails the zoom window and catches up. `tools/mag_wobble_probe.ps1
-Driver wind` run twice against the LIVE degenerate session (Wind was not restarted; the state
survived the measurement).

## Session context at capture

- Wind instance started 2026-08-20 19:28:26 local (17:28:26Z), one minute after a clean
  uiaccess_setup deploy (log: sign Valid, DONE 19:27:06). Binary: uiAccess=true manifest,
  signature Valid (Wind Dev Test Cert).
- Transform session, hybrid model, level 7.6-8.4 during capture.
- ini highlights: `txHookWrite=0` (issue #206 hook-write path OFF, all transform writes are
  tick-paced), `cursorSprite=1`, `spriteBand16=0`, `fastPan=1`, `ixDecimate=4`,
  `cursorSmoothing=0.4`, `cursorScaleWithZoom=1`, `desktopTransform=1`.
- Startup line confirms: `magthread runtime stays on the tick thread (txHookWrite off)`.

## Probe results (900 px/s constant pan, 6 s, level ~8)

Run 1 (level 8.1): view |dev| median 32.7 px, p95 49, max 1057, p2p 1602; writes 860
(143/s = tick rate); backwards writes 32 (3.7%), backwards travel 3411 px/s.
Run 2 (level 8.4): view |dev| median 34.2 px, p95 51, max 158.6, p2p 259.8; writes 860;
backwards writes 4 (0.5%), backwards travel 1366 px/s.

Run 1's extreme outliers are POLLUTED: the log shows `launch quiesce: fresh cover pwsh.exe -
transform writes held ~1.5s` at 09:45:20Z, i.e. the quiesce fired on the probe's own target
window mid-measurement and froze the view while the pan continued. Run 2 (target already
running) is the honest steady-state number.

**Sprite desync (both runs, stable):**

- Run 1: sprite |dev| median = p95 = 260.2 px; cursor-vs-content median 276.6 px, p95 309.1.
- Run 2: sprite |dev| median = p95 = 267.5 px; cursor-vs-content median 276.6 px, p95 318.5.

median == p95 means the offset is essentially CONSTANT during constant-speed pan (|dev| is the
same magnitude in both pan directions). 276 screen px at 8.4x = ~33 desktop px = ~36 ms of lag
at 900 px/s, i.e. ~5 ticks at 144 Hz. A constant-velocity lag that collapses to zero when the
pan stops is exactly the "inertia / trails then catches up" feel reported. Wind's own
`cursor divergence` log read 0-9 px throughout - it samples at the weld instant and is blind
to this artefact (which is why the probe grew the sprite metric in the first place).

The view-vs-centre median (~33 px = ~4.5 ms staleness) matches tick-paced writes exactly
(#206 measured 4.36 ms median tick latency), consistent with txHookWrite=0.

## Environment anomaly captured alongside

This instance has NEVER successfully published the input transform: every `ixwrite` line since
instance start shows fails == publishes (e.g. `publishes=36 ... fails=36`), plus one-shot
`WARN MagSetInputTransform failed (no UIAccess?) - desktop pick disabled` per session. The
PREVIOUS instance the same evening had `fails=0` (18:35:25Z, 18:46:25Z UTC). The installed
binary is correctly signed with uiAccess=true, so UIAccess is not ENGAGING for this instance -
most plausibly it was relaunched from the wrong context after the 19:27 deploy (elevated
shell, or launched by the elevated setup process directly). Consequences while it persists:
desktop hover dead zones (pointer-framework apps) and `desktopTransform` pick disabled.

## ROOT CAUSE FOUND (same day, ~13:00): native Magnifier stomps the shared input transform

Reproduced deterministically with `tools/mag_wobble_repro.ps1 -WmOpen` + `tools/mag_wobble_monitor.ps1`
(now logs `MagGetInputTransform` per second). Max's minimal recipe - start wm, leave it UNZOOMED,
zoom Wind - is confirmed, and the instrument shows exactly what flips:

- wm running at 1x publishes an ENABLED IDENTITY input transform, continuously:
  `enabled=1 src=(0,0,3840,2160) dst=(0,0,3840,2160)` - measured the whole time wm is open.
- Wind zoomed to 8.87x with wm open: the input transform STAYS identity. Wind's per-change
  publish loses the two-writer war (wm republishes continuously). Magnified desktop + enabled
  identity input transform = the documented #185 poison ("identity = dead zones, measured"),
  now ALSO shown to unmoor the visible cursor: wobble with inertia, trailing the view.
- wm closed CLEANLY (Win+Esc): input transform clears instantly (`enabled=0`), and the next pan
  shows Wind's own source rect tracking the session. Healthy. This is "closing wm fixed it".
- wm KILLED while zoomed (dirty exit): its last rect stays enabled system-wide -
  `enabled=1 src=(1280,360,3840,1800)` measured after the kill. The state survives Wind
  restarts AND a DWM restart (it lives in the input stack, not the compositor). This is
  "closing wm did NOT fix it". It clears only when a later clean publish overwrites it.
- Free-floating capture from Max's live repro: with wm open at 1x and Wind zoomed 4.81x, the
  system input transform read identity while Wind's internal `cursor divergence` read 0-2px -
  Wind cannot see this bug from inside, which is why every internal metric stayed clean.

Note on `ixwrite fails=100%`: Wind's `MagSetInputTransform` calls return FALSE in this instance
(lost UIAccess after the 2026-08-20 19:28 relaunch) yet the publishes ARE observed to take
effect (the read-back tracks Wind's session rect whenever wm is closed). The failure accounting
is misleading; verify via `MagGetInputTransform` read-back, not the return value.

A second shared-global stomp is plausible on top: wm manages `MagShowSystemCursor`, so it can
re-show the raw cursor Wind hid (the CLAUDE.md two-cursors gotcha); the DWM-magnified raw
pointer plane lags the transform during pans, which also LOOKS like a rubber-banding cursor.
Discriminator: whether the broken state shows ONE cursor or TWO (sprite + raw arrow).

Fix directions for Wind (issue to file):
1. Input-transform keep-alive: while a transform session is live, read back
   `MagGetInputTransform` each tick (~0.1ms) and republish ours whenever the read-back is not
   our rect. Wind at 144Hz beats wm's republish cadence.
2. Publish an explicit DISABLE on session end, and publish ours on session START - the start
   publish also heals any stale rect left by a dead wm.
3. Re-assert `MagShowSystemCursor(FALSE)` on the same mismatch tick if the two-cursor variant
   is confirmed.
4. Fix the ixwrite fail accounting (see note above) so the log stops crying wolf.

## Not yet done (deliberately)

- No `-Driver native` baseline: that path kills and restarts Wind, which would destroy the
  degenerate state. Run it (plus a wind rerun) after Wind is next restarted cleanly.
- No healthy-Wind baseline for the sprite metric exists yet - the sprite measurement was added
  to the probe in this round. Re-run the probe when the cursor feels correct to learn whether
  ~276 px desync is the degeneration or the (previously unmeasured) steady state.
- No fix attempted. Prime suspects to investigate, in order: how the sprite window position is
  driven (what accumulates ~5 ticks of lag), the `cursorSmoothing=0.4` pan inertia interaction,
  and whether missing UIAccess changes any of it (restart Wind from a normal shell and re-probe).
