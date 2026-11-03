<!-- Copyright (c) 2026 Max Aubert. All rights reserved.
     Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary -->
<script>
  // First-run setup, Wind for Mac (?mode=onboard). The host opens it on the first launch (onboarded
  // is false), whenever a permission Wind needs is missing, and from Run setup again
  // (AppDelegate.needsSetup). Steps:
  //   Welcome, Accessibility, Screen Recording (render engine only), macOS Zoom shortcuts (only when
  //   any were on as setup opened), Zoom keys, Done.
  // The permission steps follow the live macStatus the host pushes (bridge.js onMacStatus), so
  // Continue lights up the moment macOS grants the permission. Finishing or skipping writes
  // onboarded and switches to Settings in place; closing the window writes it on the host side.
  import { onMount } from 'svelte';
  import { getSession, setConfig, getMacStatus, onMacStatus, macAction } from './bridge.js';
  import { fill } from './session.js';
  import KeybindCapture from './lib/KeybindCapture.svelte';
  import { normalizePalette } from './design/themes.js';
  import './design/tokens.css';
  import './design/themes.css';
  let { onDone = () => {} } = $props();

  const init = window.__windInit || {};
  // The chosen theme, from the host's first-paint config like Settings. A first run has none: Wind grey.
  const palette = normalizePalette(init.values && init.values.uiPalette);
  const initMac = init.mac || null;
  const engine = String((init.values && init.values.engine) || 'zoom');

  let values = $state(fill({ values: init.values || {} }));
  let mac = $state(initMac);
  // The steps are fixed when setup opens, so turning the shortcuts off does not pull a step out from under the user.
  const steps = ['welcome', 'access',
    ...(engine === 'draw' ? ['screen'] : []),
    ...(initMac && initMac.zoomShortcuts && initMac.zoomShortcuts.length ? ['zoom'] : []),
    'keys', 'done'];
  const TITLES = { welcome: 'Welcome to Wind', access: 'Allow Accessibility', screen: 'Allow Screen Recording',
                   zoom: 'Turn off the macOS Zoom shortcuts', keys: 'Set your zoom keys', done: "You're all set" };
  // Resumed after Wind quit mid-setup (macOS's "Quit & Reopen" once Screen Recording is allowed): start at
  // the first step not done yet instead of the welcome page.
  function firstOpen() {
    if (!init.resume) return 0;
    const m = initMac || {};
    const i = steps.findIndex((s) => (s === 'access' && !m.accessibility) || (s === 'screen' && !m.screenRecording)
      || (s === 'zoom' && m.zoomShortcuts && m.zoomShortcuts.length) || s === 'keys');
    return i < 0 ? 0 : i;
  }
  let cur = $state(firstOpen());
  const step = $derived(steps[cur]);
  const N = steps.length;

  const axOk = $derived(!!(mac && mac.accessibility));
  const srOk = $derived(!!(mac && mac.screenRecording));
  const zoomOn = $derived(mac && mac.zoomShortcuts ? mac.zoomShortcuts : []);
  // Continue waits for the permission on its step; Skip this step moves on without it.
  const blocked = $derived((step === 'access' && !axOk) || (step === 'screen' && !srOk));

  onMount(() => {
    getSession().then((s) => { values = fill(s); });
    getMacStatus().then((m) => { mac = m; });
    const off = onMacStatus((m) => { mac = m; });
    // Back from System Settings: ask again (the host also pushes changes as it sees them).
    const onFocus = () => getMacStatus().then((m) => { mac = m; });
    window.addEventListener('focus', onFocus);
    return () => { off(); window.removeEventListener('focus', onFocus); };
  });

  function live(patch) {
    for (const k of Object.keys(patch)) setConfig(k, patch[k]);
    values = { ...values, ...Object.fromEntries(Object.entries(patch).map(([k, v]) => [k, String(v)])) };
  }
  function finish() { setConfig('onboarded', '1'); onDone(); }
  function next() {
    if (cur === N - 1) { finish(); return; }
    cur += 1;
  }
  function back() { if (cur > 0) cur -= 1; }
  const zoomInRow  = { key: 'zoomIn',  type: 'keybind', max: 3, label: 'Zoom in',  desc: 'Hold to zoom in',  keywords: [] };
  const zoomOutRow = { key: 'zoomOut', type: 'keybind', max: 3, label: 'Zoom out', desc: 'Hold to zoom out', keywords: [] };
  const stepMsg = $derived(`Step ${cur + 1} of ${N}: ${TITLES[step]}`);
</script>

<div class="wnd win" data-palette={palette}>
  <!-- The native traffic lights sit over this strip (the host's transparent title bar). -->
  <div class="caption" aria-hidden="true"></div>
  <main class="wizbody">
    {#if step === 'welcome'}
    <div class="step center">
      <div class="hero">
        <svg class="windsvg" viewBox="0 0 236 140" fill="none" aria-hidden="true" focusable="false">
          <defs><linearGradient id="windgrad" x1="0" y1="0" x2="1" y2="0">
            <stop offset="0" stop-color="currentColor" stop-opacity="0"/>
            <stop offset=".5" stop-color="currentColor" stop-opacity="1"/>
            <stop offset="1" stop-color="currentColor" stop-opacity="0"/>
          </linearGradient></defs>
          <path class="wln l1" pathLength="100" d="M246 34 C 178 8, 96 54, -10 30"/>
          <path class="wln l2" pathLength="100" d="M246 60 C 168 94, 82 32, -10 66"/>
          <path class="wln l3" pathLength="100" d="M246 80 C 188 60, 72 114, -10 86"/>
          <path class="wln l4" pathLength="100" d="M246 48 C 162 58, 62 40, -10 54"/>
          <path class="wln l5" pathLength="100" d="M246 108 C 172 130, 86 96, -10 112"/>
        </svg>
        <svg class="logosvg" viewBox="0 0 16 16" fill="none" aria-hidden="true" focusable="false">
          <path class="lp" pathLength="100" d="M2 5.5h8.5a2 2 0 1 0-2-2"/>
          <path class="lp" pathLength="100" d="M2 9h11a2 2 0 1 1-2 2"/>
          <path class="lp" pathLength="100" d="M2 12.5h6.5a1.7 1.7 0 1 1-1.7 1.7"/>
        </svg>
      </div>
      <h1>Welcome to Wind</h1>
      <p>A few quick steps to get started</p>
    </div>
    {:else if step === 'access'}
    <div class="step" data-step="access">
      <h1>Allow Accessibility</h1>
      <p>Wind needs it to see your zoom keys and move the view with the pointer. Turn Wind on in the list, then come back here.</p>
      <div class="orow">
        <div class="ot"><div class="rlabel">Accessibility</div>
          <div class="st" class:ok={axOk} role="status">{axOk ? 'Allowed' : 'Not allowed yet'}</div></div>
        {#if !axOk}<div class="rctl"><button type="button" class="btn" onclick={() => macAction('openAccessibility')}>Open System Settings</button></div>{/if}
      </div>
    </div>
    {:else if step === 'screen'}
    <div class="step" data-step="screen">
      <h1>Allow Screen Recording</h1>
      <p>For the Render engine, Wind's default: it reads the screen and draws the zoomed view itself, and macOS calls that Screen Recording. Nothing is recorded or saved. macOS may ask you to reopen Wind after you allow it.</p>
      <p class="onote">Only Render needs this. The Transform engine (Settings, Zoom, Engine) works without it.</p>
      <div class="orow">
        <div class="ot"><div class="rlabel">Screen Recording</div>
          <div class="st" class:ok={srOk} role="status">{srOk ? 'Allowed' : 'Not allowed yet'}</div></div>
        {#if !srOk}<div class="rctl"><button type="button" class="btn" onclick={() => macAction('openScreenRecording')}>Open System Settings</button></div>{/if}
      </div>
    </div>
    {:else if step === 'zoom'}
    <div class="step" data-step="zoom">
      <h1>Turn off the macOS Zoom shortcuts</h1>
      <p>The zoom built into macOS fights Wind for the screen while its shortcuts are on. Wind can switch them off for you.</p>
      <div class="orow">
        <div class="ot"><div class="rlabel">macOS Zoom shortcuts</div>
          <div class="st" class:ok={!zoomOn.length} role="status">{zoomOn.length ? 'On: ' + zoomOn.join(', ') : 'All off'}</div></div>
        {#if zoomOn.length}<div class="rctl"><button type="button" class="btn" onclick={() => macAction('fixZoomShortcuts')}>Turn off</button></div>{/if}
      </div>
    </div>
    {:else if step === 'keys'}
    <div class="step" data-step="keys">
      <h1>Set your zoom keys</h1>
      <p>Pick the buttons or keys you hold to zoom. ⌃ Control + scroll zooms already.</p>
      <div class="orow"><div class="ot"><div class="rlabel" id="ob-in-l">Zoom in</div><div class="rdesc" id="ob-in-d">Hold to zoom in</div></div>
        <div class="rctl"><KeybindCapture row={zoomInRow} {values} onChange={live}
                            labelledby="ob-in-l ob-in-v" describedby="ob-in-d" valueId="ob-in-v" /></div></div>
      <div class="orow"><div class="ot"><div class="rlabel" id="ob-out-l">Zoom out</div><div class="rdesc" id="ob-out-d">Hold to zoom out</div></div>
        <div class="rctl"><KeybindCapture row={zoomOutRow} {values} onChange={live}
                            labelledby="ob-out-l ob-out-v" describedby="ob-out-d" valueId="ob-out-v" /></div></div>
    </div>
    {:else}
    <div class="step center" data-step="done">
      <div class="bigring"><svg viewBox="0 0 80 80" width="86" height="86" aria-hidden="true" focusable="false">
        <circle class="ring" cx="40" cy="40" r="36" fill="none" stroke="var(--fg)" stroke-width="3"/>
        <path class="tick" d="M25 41l10 10 20-21" fill="none" stroke="var(--fg)" stroke-width="3.5" stroke-linecap="round" stroke-linejoin="round"/>
      </svg></div>
      <h1>You're all set</h1>
      <p>Wind lives in the menu bar. Settings has everything else.</p>
    </div>
    {/if}
  </main>
  <div class="wizdots" aria-hidden="true">{#each steps as s, i (s)}<i class:on={i === cur}></i>{/each}</div>
  <div class="sr-only" role="status" aria-live="polite" aria-atomic="true">{stepMsg}</div>
  <div class="wizfoot">
    {#if cur < N - 1}<button class="skip" type="button" onclick={finish}>Skip setup</button>{/if}
    {#if cur > 0}<button class="btn" type="button" onclick={back}>Back</button>{/if}
    {#if blocked}<button class="btn" type="button" onclick={next}>Skip this step</button>{/if}
    <button class="btn primary" type="button" disabled={blocked} onclick={next}>{cur === 0 ? 'Get started' : cur === N - 1 ? 'Open Settings' : 'Continue'}</button>
  </div>
</div>

<style>
  .win { width: 100vw; height: 100vh; overflow: hidden; display: flex; flex-direction: column;
         background: var(--bg); color: var(--fg); font-size: 13px; }
  .caption { height: 40px; flex-shrink: 0; }

  .wizbody { flex: 1; overflow: auto; display: flex; flex-direction: column; justify-content: center; align-items: center; padding: 8px 52px; }
  .step { width: 100%; max-width: 460px; }
  .step h1 { font-size: 27px; margin: 0 0 10px; font-weight: 600; letter-spacing: -.3px; }
  .step p { color: var(--fg3); font-size: 13.5px; line-height: 1.55; margin: 0 0 24px; }
  .step p:has(+ .onote) { margin-bottom: 8px; }
  .step p.onote { font-size: 12.5px; }
  .step.center { text-align: center; }
  .step.center p { max-width: 384px; margin-left: auto; margin-right: auto; }

  .bigring { width: 88px; height: 88px; margin: 0 auto 22px; }
  .ring { stroke-dasharray: 227; stroke-dashoffset: 227; transform: rotate(-90deg); transform-origin: 50% 50%; animation: ringDraw .9s ease forwards; }
  .tick { stroke-dasharray: 44; stroke-dashoffset: 44; animation: tickDraw .45s ease .6s forwards; }
  @keyframes ringDraw { to { stroke-dashoffset: 0; } }
  @keyframes tickDraw { to { stroke-dashoffset: 0; } }

  /* welcome: wind trails flow, then the logo draws in and settles static */
  .hero { position: relative; width: 236px; height: 140px; margin: 0 auto 14px; display: grid; place-items: center; }
  .windsvg { position: absolute; inset: 0; width: 236px; height: 140px; color: var(--fg2); animation: trailsOut .9s ease-out .85s forwards; }
  .wln { fill: none; stroke: url(#windgrad); stroke-linecap: round; stroke-dasharray: 26 200; }
  .wln.l1 { stroke-width: 2.6; animation: windTrail 3.2s linear infinite; }
  .wln.l2 { stroke-width: 2.2; animation: windTrail 4.2s linear .7s infinite; }
  .wln.l3 { stroke-width: 3;   animation: windTrail 3.6s linear 1.3s infinite; }
  .wln.l4 { stroke-width: 2;   animation: windTrail 4.7s linear .3s infinite; }
  .wln.l5 { stroke-width: 2.4; animation: windTrail 3.9s linear 1.9s infinite; }
  @keyframes windTrail { from { stroke-dashoffset: 0; } to { stroke-dashoffset: -226; } }
  @keyframes trailsOut { to { opacity: 0; } }
  .logosvg { position: relative; z-index: 1; width: 84px; height: 84px; color: var(--fg); opacity: 0; animation: logoIn .6s ease-out .95s forwards; }
  @keyframes logoIn { from { opacity: 0; transform: scale(.94); } to { opacity: 1; transform: scale(1); } }
  .lp { fill: none; stroke: currentColor; stroke-width: 1.2; stroke-linecap: round; stroke-dasharray: 100; stroke-dashoffset: -100; animation: logoDraw .8s ease-out forwards; }
  .lp:nth-child(1) { animation-delay: .95s; }
  .lp:nth-child(2) { animation-delay: 1.05s; }
  .lp:nth-child(3) { animation-delay: 1.15s; }
  @keyframes logoDraw { to { stroke-dashoffset: 0; } }

  .orow { display: flex; align-items: center; gap: 16px; padding: 15px 16px; border: 1px solid var(--cardb); background: var(--card); box-shadow: var(--cshadow); border-radius: var(--rad); margin-bottom: 11px; }
  .orow > .ot { flex: 1; min-width: 0; }
  .rlabel { font-size: 13.5px; } .rdesc { font-size: 11.5px; color: var(--fg3); margin-top: 2px; }
  .rctl { flex-shrink: 0; display: flex; align-items: center; gap: 10px; }
  .st { margin-top: 3px; font-size: 12px; color: var(--danger); }
  .st.ok { color: var(--fill); }

  .wizdots { display: flex; justify-content: center; gap: 7px; padding: 4px 0 10px; }
  .wizdots i { width: 7px; height: 7px; border-radius: 50%; background: var(--track); transition: width var(--dur) var(--ease), background-color var(--dur) var(--ease); }
  .wizdots i.on { width: 22px; border-radius: 4px; background: var(--accent); }

  .wizfoot { flex-shrink: 0; display: flex; align-items: center; gap: 10px; padding: 14px 24px; }
  .skip { margin-right: auto; background: transparent; border: 0; color: var(--fg3); font-size: 12.5px; cursor: pointer; }
  .skip:hover { color: var(--fg); }
  .btn { height: 36px; padding: 0 20px; border-radius: var(--rp); border: 1px solid var(--chipb); background: transparent; color: var(--fg2); font: 600 13px var(--s); cursor: pointer; }
  .btn:hover:not(:disabled) { border-color: var(--outline); color: var(--fg); }
  .btn.primary { background: var(--accent); border-color: var(--accent); color: var(--onaccent); }
  /* The plain hover above outranks .primary: keep its text on the accent, show the hover as a dim. */
  .btn.primary:hover:not(:disabled) { border-color: var(--accent); color: var(--onaccent); opacity: .86; }
  .btn:disabled { opacity: .4; cursor: default; }
  .wizfoot .btn:first-child { margin-left: auto; }
  /* KeybindCapture: restyle its keycap with the tokens here. */
  .rctl :global(.keycap) { font: 500 12px var(--m); color: var(--fg); background: var(--chip); border: 1px solid var(--chipb); border-radius: var(--srad); padding: 5px 10px; min-height: 28px; }
  .rctl :global(.keycap:hover) { border-color: var(--outline); }
  .rctl :global(.keycap.armed) { border-color: var(--fg); background: var(--hover); }
  .rctl :global(.refusal) { color: var(--fg2); }
  .sr-only { position: absolute; width: 1px; height: 1px; margin: -1px; padding: 0; overflow: hidden;
             clip: rect(0 0 0 0); white-space: nowrap; border: 0; }
</style>
