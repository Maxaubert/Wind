<script>
  import { iconSvg } from '../design/icons.js';
  // Props only. The bar carries the window buttons and nothing else: minimize, maximize (restore when
  // maximized), close.
  let { title = 'Wind Settings', maximized = false, onMinimize = () => {}, onMaximize = () => {}, onClose = () => {} } = $props();
</script>

<header class="tb">
  <div class="brand"><span class="logo">{@html iconSvg('logo')}</span>{title}</div>
  <div class="sp"></div>
  <button class="wc" type="button" aria-label="Minimize" onclick={() => onMinimize()}>{@html iconSvg('minimize')}</button>
  <button class="wc" type="button" aria-label={maximized ? 'Restore' : 'Maximize'} onclick={() => onMaximize()}>{@html iconSvg('maximize')}</button>
  <button class="wc" type="button" aria-label="Close" onclick={() => onClose()}>{@html iconSvg('close')}</button>
</header>

<style>
  /* The bar drags the window (WebView2 non-client regions, enabled by the host); the buttons opt out. */
  .tb { display: flex; align-items: center; height: 38px; border-bottom: 1px solid var(--line); padding-left: 12px; background: var(--bg);
        app-region: drag; -webkit-app-region: drag; }
  .tb button { app-region: no-drag; -webkit-app-region: no-drag; }
  .brand { display: flex; align-items: center; gap: 10px; font: 600 13px var(--s); }
  .logo { width: 20px; height: 20px; border-radius: 4px; background: var(--fg); color: var(--bg); display: grid; place-items: center; }
  .logo :global(svg.ic) { width: 12px; height: 12px; stroke-width: 2; }
  .sp { flex: 1; }
  .wc { width: 46px; height: 38px; display: grid; place-items: center; color: var(--fg2); }
  .wc:hover { background: var(--hover); color: var(--fg); }
  .wc :global(svg.ic) { width: 15px; height: 15px; stroke-width: 1.75; }
</style>
