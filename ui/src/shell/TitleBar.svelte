<script>
  import { iconSvg } from '../design/icons.js';
  // Props only. The bar carries the window buttons and nothing else: minimize, maximize (restore when
  // maximized), close.
  // Not a <header> (#514): a banner landmark holding "Wind Settings" and window buttons at the top of
  // the document read to NVDA as a second window laid over the real one. The brand repeats the window
  // title NVDA already speaks, so it is hidden from assistive tech. Settings renders this bar AFTER the
  // page in the DOM (grid-row puts it back on top), so the buttons come last in Tab and reading order.
  let { title = 'Wind Settings', maximized = false, onMinimize = () => {}, onMaximize = () => {}, onClose = () => {} } = $props();
</script>

<div class="tb">
  <div class="brand" aria-hidden="true"><span class="logo">{@html iconSvg('logo')}</span>{title}</div>
  <div class="sp"></div>
  <button class="wc" type="button" aria-label="Minimize" onclick={() => onMinimize()}>{@html iconSvg('minimize')}</button>
  <button class="wc" type="button" aria-label={maximized ? 'Restore' : 'Maximize'} onclick={() => onMaximize()}>{@html iconSvg('maximize')}</button>
  <button class="wc" type="button" aria-label="Close" onclick={() => onClose()}>{@html iconSvg('close')}</button>
</div>

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
