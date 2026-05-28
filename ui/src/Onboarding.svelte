<script>
  import { setConfig, windowControl } from './bridge.js';
  import { ic } from './lib/icons.js';
  import KeybindCapture from './lib/KeybindCapture.svelte';
  export let onDone;
  let cur = 0;
  const N = 3;
  // Keybinds start blank (Unbound). The KeybindCapture below writes setConfig live as the user
  // captures, so untouched bindings keep whatever the ini already had (defaults for a fresh ini).
  let keys = { zoomInButton:'0', zoomInVk:'0', zoomOutButton:'0', zoomOutVk:'0',
               zoomInMods:'0', zoomOutMods:'0' };
  function live(patch) {
    for (const k of Object.keys(patch)) setConfig(k, patch[k]);
    keys = { ...keys, ...patch };
  }
  function next() {
    if (cur === N - 1) { setConfig('onboarded', '1'); onDone(); return; }
    cur += 1;
  }
  function back() { if (cur > 0) cur -= 1; }
  function skip() { setConfig('onboarded', '1'); onDone(); }
  const zoomInRow  = { label:'Zoom in',  desc:'Hold to magnify',  buttonKey:'zoomInButton',  vkKey:'zoomInVk',  modsKey:'zoomInMods' };
  const zoomOutRow = { label:'Zoom out', desc:'Hold to zoom back', buttonKey:'zoomOutButton', vkKey:'zoomOutVk', modsKey:'zoomOutMods' };
</script>
<div class="win">
  <div class="caption" style="app-region:drag;-webkit-app-region:drag">
    <div class="tbtns" style="app-region:no-drag;-webkit-app-region:no-drag">
      <button class="tbtn" title="Minimize" aria-label="Minimize" on:click={() => windowControl('minimize')}>{@html ic.min}</button>
      <button class="tbtn close" title="Close" aria-label="Close" on:click={() => windowControl('close')}>{@html ic.close}</button>
    </div>
  </div>
  <div class="wizbody">
    <!-- Step 0: Welcome. Ported .hero block (windsvg trails + windgrad def + logosvg). -->
    <div class="step center" class:show={cur === 0}>
      <div class="hero">
        <svg class="windsvg" viewBox="0 0 236 140" fill="none">
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
        <svg class="logosvg" viewBox="0 0 16 16" fill="none">
          <path class="lp" pathLength="100" d="M2 5.5h8.5a2 2 0 1 0-2-2"/>
          <path class="lp" pathLength="100" d="M2 9h11a2 2 0 1 1-2 2"/>
          <path class="lp" pathLength="100" d="M2 12.5h6.5a1.7 1.7 0 1 1-1.7 1.7"/>
        </svg>
      </div>
      <h2>Welcome to Wind</h2>
      <p>A fast magnifier that lives in your tray. Let's set up the essentials.</p>
    </div>
    <!-- Step 1: Set your zoom keys -->
    <div class="step" class:show={cur === 1}>
      <h2>Set your zoom keys</h2>
      <p>Pick the buttons you'll hold to zoom. Mouse side-buttons work great, or choose keyboard keys.</p>
      <div class="orow"><div class="ot"><div class="rlabel">Zoom in</div><div class="rdesc">Hold to magnify</div></div>
        <div class="rctl"><KeybindCapture row={zoomInRow} values={keys} onChange={live} /></div></div>
      <div class="orow"><div class="ot"><div class="rlabel">Zoom out</div><div class="rdesc">Hold to zoom back</div></div>
        <div class="rctl"><KeybindCapture row={zoomOutRow} values={keys} onChange={live} /></div></div>
    </div>
    <!-- Step 2: You're all set. Ported .bigring check-ring SVG. -->
    <div class="step center" class:show={cur === 2}>
      <div class="bigring"><svg viewBox="0 0 80 80" width="86" height="86">
        <circle class="ring" cx="40" cy="40" r="36" fill="none" stroke="var(--accent)" stroke-width="3"/>
        <path class="tick" d="M25 41l10 10 20-21" fill="none" stroke="var(--accent)" stroke-width="3.5" stroke-linecap="round" stroke-linejoin="round"/>
      </svg></div>
      <h2>You're all set</h2>
    </div>
  </div>
  <div class="wizdots">{#each Array(N) as _, i}<i class:on={i === cur}></i>{/each}</div>
  <div class="wizfoot">
    {#if cur < N - 1}<button class="skip" on:click={skip}>Skip setup</button>{/if}
    {#if cur > 0}<button class="btn" on:click={back}>Back</button>{/if}
    <button class="btn primary" on:click={next}>{cur === 0 ? 'Get started' : cur === N - 1 ? 'Open Settings' : 'Next'}</button>
  </div>
</div>
<style>
  /* Ported from mockups/config-ui-onboarding.html. The .win is a full real window (100vw/100vh),
     not the mockup's fixed 560x500 demo card. Theme tokens (--bg/--text/--accent/...) come from
     the global theme.css. Animated elements all live here, so .step.show gating drives them. */
  .win { width: 100vw; height: 100vh; overflow: hidden; display: flex; flex-direction: column;
         background: var(--bg); color: var(--text); font-size: 13px; }

  .caption { height: 44px; flex-shrink: 0; display: flex; align-items: center; justify-content: flex-end; padding-left: 10px; }
  .tbtns { display: flex; height: 100%; }
  .tbtn { width: 46px; height: 100%; display: grid; place-items: center; color: var(--muted); border: 0; background: transparent; cursor: pointer; }
  .tbtn:hover { background: var(--hover); color: var(--text); }
  .tbtn.close:hover { background: #e81123; color: #fff; }

  .wizbody { flex: 1; overflow: auto; display: flex; flex-direction: column; justify-content: center; align-items: center; padding: 8px 52px; }
  .step { display: none; width: 100%; max-width: 430px; }
  .step.show { display: block; }
  .step h2 { font-size: 27px; margin: 0 0 10px; font-weight: 600; letter-spacing: -.3px; }
  .step p { color: var(--muted); font-size: 13.5px; line-height: 1.55; margin: 0 0 24px; }
  .step.center { text-align: center; }
  .step.center p { max-width: 384px; margin-left: auto; margin-right: auto; }

  .bigring { width: 88px; height: 88px; margin: 0 auto 22px; }
  .ring { stroke-dasharray: 227; stroke-dashoffset: 227; transform: rotate(-90deg); transform-origin: 50% 50%; }
  .tick { stroke-dasharray: 44; stroke-dashoffset: 44; }
  @keyframes ringDraw { to { stroke-dashoffset: 0; } }
  @keyframes tickDraw { to { stroke-dashoffset: 0; } }
  .step.show .ring { animation: ringDraw .9s ease forwards; }
  .step.show .tick { animation: tickDraw .45s ease .6s forwards; }

  /* welcome: wind trails flow, then the logo draws in and settles static */
  .hero { position: relative; width: 236px; height: 140px; margin: 0 auto 14px; display: grid; place-items: center; }
  .windsvg { position: absolute; inset: 0; width: 236px; height: 140px; color: var(--accent-icon); }
  .step.show .windsvg { animation: trailsOut .9s ease-out .85s forwards; }
  .wln { fill: none; stroke: url(#windgrad); stroke-linecap: round; stroke-dasharray: 26 200; }
  .wln.l1 { stroke-width: 2.6; animation: windTrail 3.2s linear infinite; }
  .wln.l2 { stroke-width: 2.2; animation: windTrail 4.2s linear .7s infinite; }
  .wln.l3 { stroke-width: 3;   animation: windTrail 3.6s linear 1.3s infinite; }
  .wln.l4 { stroke-width: 2;   animation: windTrail 4.7s linear .3s infinite; }
  .wln.l5 { stroke-width: 2.4; animation: windTrail 3.9s linear 1.9s infinite; }
  @keyframes windTrail { from { stroke-dashoffset: 0; } to { stroke-dashoffset: -226; } }
  @keyframes trailsOut { to { opacity: 0; } }
  .logosvg { position: relative; z-index: 1; width: 84px; height: 84px; color: var(--accent-icon); opacity: 0; }
  .step.show .logosvg { animation: logoIn .6s ease-out .95s forwards; }
  @keyframes logoIn { from { opacity: 0; transform: scale(.94); } to { opacity: 1; transform: scale(1); } }
  .lp { fill: none; stroke: currentColor; stroke-width: 1.2; stroke-linecap: round; stroke-dasharray: 100; stroke-dashoffset: -100; }
  .step.show .lp { animation: logoDraw .8s ease-out forwards; }
  .step.show .lp:nth-child(1) { animation-delay: .95s; }
  .step.show .lp:nth-child(2) { animation-delay: 1.05s; }
  .step.show .lp:nth-child(3) { animation-delay: 1.15s; }
  @keyframes logoDraw { to { stroke-dashoffset: 0; } }

  .orow { display: flex; align-items: center; gap: 16px; padding: 15px 16px; border: 1px solid var(--line); border-radius: 10px; margin-bottom: 11px; }
  .orow > .ot { flex: 1; min-width: 0; }
  .rlabel { font-size: 13.5px; } .rdesc { font-size: 11.5px; color: var(--muted); margin-top: 2px; }
  .rctl { flex-shrink: 0; display: flex; align-items: center; gap: 10px; }

  .wizdots { display: flex; justify-content: center; gap: 7px; padding: 4px 0 10px; }
  .wizdots i { width: 7px; height: 7px; border-radius: 50%; background: var(--track); transition: width .2s; }
  .wizdots i.on { width: 22px; border-radius: 4px; background: var(--accent); }

  .wizfoot { flex-shrink: 0; display: flex; align-items: center; gap: 10px; padding: 14px 24px; border-top: 1px solid var(--line); }
  .skip { margin-right: auto; background: transparent; border: 0; color: var(--muted); font-size: 12.5px; cursor: pointer; }
  .skip:hover { color: var(--text); }
  .btn { padding: 8px 20px; border-radius: 7px; border: 1px solid var(--line); background: transparent; color: var(--text); font-size: 12.5px; cursor: pointer; }
  .btn.primary { background: var(--accent); border-color: var(--accent); color: #fff; }
</style>
