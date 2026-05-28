<script>
  import { onMount } from 'svelte';
  export let onDone = () => {};
  // Animation timeline: trails flow 0-1s, fade out .85-1.75s; logo fades + draws 0.95-1.95s.
  // Hold a beat after it settles, then signal the parent so it can transition into the real UI.
  onMount(() => {
    const t = setTimeout(onDone, 2400);
    return () => clearTimeout(t);
  });
</script>
<div class="splash">
  <div class="hero">
    <svg class="windsvg" viewBox="0 0 236 140" fill="none">
      <defs><linearGradient id="splashgrad" x1="0" y1="0" x2="1" y2="0">
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
</div>
<style>
  /* Same wind-trails-into-logo motion as the Onboarding Welcome hero; runs on mount without the
     .step.show gate since this whole component mounts and unmounts atomically. */
  .splash { width: 100vw; height: 100vh; display: grid; place-items: center;
            background: var(--bg); color: var(--accent-icon); }
  .hero { position: relative; width: 236px; height: 140px; display: grid; place-items: center; }
  .windsvg { position: absolute; inset: 0; width: 236px; height: 140px;
             animation: trailsOut .9s ease-out .85s forwards; }
  .wln { fill: none; stroke: url(#splashgrad); stroke-linecap: round; stroke-dasharray: 26 200; }
  .wln.l1 { stroke-width: 2.6; animation: windTrail 3.2s linear infinite; }
  .wln.l2 { stroke-width: 2.2; animation: windTrail 4.2s linear .7s infinite; }
  .wln.l3 { stroke-width: 3;   animation: windTrail 3.6s linear 1.3s infinite; }
  .wln.l4 { stroke-width: 2;   animation: windTrail 4.7s linear .3s infinite; }
  .wln.l5 { stroke-width: 2.4; animation: windTrail 3.9s linear 1.9s infinite; }
  @keyframes windTrail { from { stroke-dashoffset: 0; } to { stroke-dashoffset: -226; } }
  @keyframes trailsOut { to { opacity: 0; } }
  .logosvg { position: relative; z-index: 1; width: 84px; height: 84px; opacity: 0;
             animation: logoIn .6s ease-out .95s forwards; }
  @keyframes logoIn { from { opacity: 0; transform: scale(.94); } to { opacity: 1; transform: scale(1); } }
  .lp { fill: none; stroke: currentColor; stroke-width: 1.2; stroke-linecap: round;
        stroke-dasharray: 100; stroke-dashoffset: -100; animation: logoDraw .8s ease-out forwards; }
  .lp:nth-child(1) { animation-delay: .95s; }
  .lp:nth-child(2) { animation-delay: 1.05s; }
  .lp:nth-child(3) { animation-delay: 1.15s; }
  @keyframes logoDraw { to { stroke-dashoffset: 0; } }
</style>
