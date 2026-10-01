<script>
  import { iconSvg } from '../design/icons.js';
  // Props only. themeMode is 'auto' | 'light' | 'dark'; onTheme receives the NEXT mode
  // (cycle dark -> light -> auto, as in the mockup).
  let { title = 'Wind Settings', themeMode = 'auto', onTheme = () => {}, onMinimize = () => {}, onClose = () => {} } = $props();
  const LABEL = { auto: 'Auto', light: 'Light', dark: 'Dark' };
  const ORDER = ['dark', 'light', 'auto'];
  const next = () => ORDER[(ORDER.indexOf(themeMode) + 1) % ORDER.length];
</script>

<header class="tb">
  <div class="brand"><span class="logo">{@html iconSvg('logo')}</span>{title}</div>
  <div class="sp"></div>
  <button class="wc" type="button" data-theme-cycle title="Theme: {LABEL[themeMode]} (click to change)"
          aria-label="Theme: {LABEL[themeMode]}" onclick={() => onTheme(next())}>{@html iconSvg(themeMode)}</button>
  <button class="wc" type="button" aria-label="Minimize" onclick={() => onMinimize()}>{@html iconSvg('minimize')}</button>
  <button class="wc" type="button" aria-label="Close" onclick={() => onClose()}>{@html iconSvg('close')}</button>
</header>

<style>
  .tb { display: flex; align-items: center; height: 38px; border-bottom: 1px solid var(--line); padding-left: 12px; background: var(--bg); }
  .brand { display: flex; align-items: center; gap: 10px; font: 600 13px var(--s); }
  .logo { width: 20px; height: 20px; border-radius: 4px; background: var(--fg); color: var(--bg); display: grid; place-items: center; }
  .logo :global(svg) { width: 12px; height: 12px; stroke-width: 2; }
  .sp { flex: 1; }
  .wc { width: 46px; height: 38px; display: grid; place-items: center; color: var(--fg2); }
  .wc:hover { background: var(--hover); color: var(--fg); }
  .wc :global(svg) { width: 15px; height: 15px; stroke-width: 1.75; }
  .wc[data-theme-cycle] :global(svg) { width: 14px; height: 14px; stroke-width: 1.5; }
</style>
