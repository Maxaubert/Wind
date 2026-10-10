<script>
  // The two item lists of the "Tray menu" tab: Sliders (at most four on) and Toggles. The
  // Performance switch above them is an ordinary schema row. Each change writes that list's two ini
  // keys through onChange(key, value); the page owns nothing else. Spec 2026-10-01-tray-flyout.
  import { iconSvg } from '../design/icons.js';
  import DragList from './DragList.svelte';
  import { parseList, serialiseList, onCount, MAX_SLIDERS } from './trayModel.js';
  let { values = {}, onChange = () => {}, announce = () => {} } = $props();

  const sliders = $derived(parseList(values, 'sliders'));
  const toggles = $derived(parseList(values, 'toggles'));
  const sliderCount = $derived(onCount(sliders));
  const full = $derived(sliderCount >= MAX_SLIDERS);

  const write = (kind, items) => { const w = serialiseList(kind, items); for (const k of Object.keys(w)) onChange(k, w[k]); };
  function flip(kind, items, it) {
    if (!it) return;
    if (kind === 'sliders' && !it.on && full) return;   // at the cap: unchecked rows stay off
    write(kind, items.map((i) => (i.key === it.key ? { ...i, on: !i.on } : i)));
    announce(it.name + (it.on ? ' hidden' : ' shown'));
  }
  function reorder(kind, items, keysInOrder) {
    write(kind, keysInOrder.map((k) => items.find((i) => i.key === k)));
  }
  const blocked = (kind, it) => kind === 'sliders' && !it.on && full;
</script>

<div class="thint">Check items to show them and drag to reorder</div>

{#snippet itemRow(kind, it)}
  <span class="tic" title={it.name}>{@html iconSvg(it.icon, 24)}</span>
  <div class="txt">
    <div class="k" class:off={!it.on}>{it.name}</div>
    <div class="d">{it.desc}</div>
  </div>
  <button type="button" class="chk" role="checkbox" aria-checked={it.on} disabled={blocked(kind, it)}
          aria-label={'Show ' + it.name + ' in the tray'}>
    <svg viewBox="0 0 16 16" aria-hidden="true"><path d="M3 8.5l3.25 3.25L13 4.5"/></svg>
  </button>
{/snippet}

<div class="tcap">
  <span id="cap-sliders">Sliders</span>
  <span class="cnt" data-cnt="sliders">{sliderCount} of {MAX_SLIDERS}{full ? ' full' : ''}</span>
</div>
<DragList items={sliders} labelledby="cap-sliders" {announce}
          onRowClick={(it) => flip('sliders', sliders, it)}
          onReorder={(k) => reorder('sliders', sliders, k)}>
  {#snippet row(it)}{@render itemRow('sliders', it)}{/snippet}
</DragList>

<div class="tcap">
  <span id="cap-toggles">Toggles</span>
  <span class="cnt" data-cnt="toggles">{onCount(toggles)} on</span>
</div>
<DragList items={toggles} labelledby="cap-toggles" {announce}
          onRowClick={(it) => flip('toggles', toggles, it)}
          onReorder={(k) => reorder('toggles', toggles, k)}>
  {#snippet row(it)}{@render itemRow('toggles', it)}{/snippet}
</DragList>

<style>
  .thint { margin: 14px 2px 0; font: 12.5px var(--s); color: var(--fg3); }
  .tcap { display: flex; align-items: baseline; justify-content: space-between; margin: 16px 2px 8px;
          font: 500 13px var(--s); letter-spacing: .02em; color: var(--fg3); }
  .thint + .tcap { margin-top: 16px; }
  :global(.tlist) + .tcap { margin-top: 26px; }
  .cnt { font: 400 11.5px var(--m); letter-spacing: 0; color: var(--fg3); opacity: .85; font-variant-numeric: tabular-nums; }
  .tic { width: 24px; height: 36px; display: grid; place-items: center; color: var(--fg2); flex: none; }
  .tic :global(svg.ic) { width: 24px; height: 24px; stroke-width: 1; stroke-linecap: square; stroke-linejoin: miter; }
  .txt { min-width: 0; }
  .k { font: 500 13.5px var(--s); color: var(--fg); }
  .k.off { color: var(--fg2); }
  .d { margin-top: 1px; font: 12.5px var(--s); color: var(--fg3); }
  .chk { width: 28px; height: 28px; display: grid; place-items: center; border-radius: 7px;
         border: 1px solid var(--ctl); background: transparent; color: transparent;
         transition: background-color var(--dur-fast) var(--ease), border-color var(--dur-fast) var(--ease), transform var(--dur-fast) var(--ease); }
  .chk:active:not(:disabled) { transform: scale(.97); }
  .chk svg { width: 16px; height: 16px; fill: none; stroke: currentColor; stroke-width: 2; stroke-linecap: square; stroke-linejoin: miter; }
  .chk:hover { border-color: var(--fg); }
  .chk[aria-checked="true"] { color: var(--onfill); background: var(--fill); border-color: var(--fill); }
  .chk[aria-checked="true"] svg { stroke-width: 2.25; }
  .chk[aria-checked="true"]:hover { filter: brightness(1.1); }
  .chk:disabled { cursor: not-allowed; opacity: .5; }
  .chk:disabled:hover { filter: none; border-color: var(--ctl); }
</style>
