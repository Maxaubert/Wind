<script>
  import { onMount } from 'svelte';
  import { iconSvg } from '../design/icons.js';
  // Page banner: near-black band, outlined rounded icon box, aurora still fading in from the right.
  let { title = '', description = '', icon = '', image = './c-grey.jpg' } = $props();
  // The aurora URL is only set two frames after mount, so the image request cannot start before the
  // banner's first render. Measured: no first-paint change.
  let loaded = $state(false);
  onMount(() => { requestAnimationFrame(() => requestAnimationFrame(() => { loaded = true; })); });
</script>

<header class="banner">
  <!-- A real element with an inline background-image: a url() inside a CSS custom property is resolved
       against the stylesheet (dist/assets/), not the page, and would request /assets/c-grey.jpg. -->
  <div class="aurora" aria-hidden="true" style={loaded ? `background-image:url(${image})` : ''}></div>
  <div class="tint" aria-hidden="true"></div>
  {#if icon}<span class="bico">{@html iconSvg(icon, 22)}</span>{/if}
  <div>
    <h1>{title}</h1>
    {#if description}<p class="bdesc">{description}</p>{/if}
  </div>
</header>

<style>
  .banner { margin: 0 calc(-1 * var(--gx, 40px)); padding: 28px var(--gx, 40px); display: flex; align-items: center; gap: 18px; position: relative;
            background-color: var(--band); border-bottom: 1px solid var(--line); isolation: isolate; }
  .aurora { position: absolute; inset: 0; pointer-events: none;
    background: right center / cover no-repeat; opacity: var(--bandimg-op); filter: var(--bnfilter);
    -webkit-mask-image: linear-gradient(90deg, transparent var(--bandimg-from), #000 calc(var(--bandimg-from) + 30%));
    mask-image: linear-gradient(90deg, transparent var(--bandimg-from), #000 calc(var(--bandimg-from) + 30%)); }
  /* The theme colour wash, blended onto the image (themes set --bntint and --bntop; Wind grey has none). */
  .tint { position: absolute; inset: 0; pointer-events: none; background: var(--bntint); mix-blend-mode: color;
    opacity: var(--bntop);
    -webkit-mask-image: linear-gradient(90deg, transparent var(--bandimg-from), #000 calc(var(--bandimg-from) + 30%));
    mask-image: linear-gradient(90deg, transparent var(--bandimg-from), #000 calc(var(--bandimg-from) + 30%)); }
  .banner > :not(.aurora):not(.tint) { position: relative; }
  .banner > div { min-width: 0; }
  h1 { margin: 0; overflow-wrap: break-word; font: 600 28px/1.1 var(--s); letter-spacing: -.4px; }
  .bdesc { margin: 6px 0 0; text-wrap: pretty; color: var(--fg3); font: 13.5px var(--s); }
  .bico { width: 48px; height: 48px; display: grid; place-items: center; flex: none;
          border: 1px solid var(--line2); border-radius: var(--rc); }
</style>
