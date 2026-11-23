<!-- Copyright (c) 2026 Max Aubert. All rights reserved.
     Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary -->
<script>
  // The menu bar panel (Sources/Wind/MenuBarPanel.swift hosts it): Windows' tray flyout on the Mac, in
  // its order, sizes and tokens (src/tray_app/flyout_model.h ComputeGeometry, flyout_draw.cpp,
  // flyout_palettes.h): the Performance header, up to four sliders, the toggle group and the engine
  // picker, then Settings and Quit. Mac-only banners sit on top, only while needed. What it shows and in
  // what order is Settings > Menu bar (menu* keys). Every change is written at once through the host.
  import '$shared/design/themes.css';
  import '../design/tokens.css';
  import './palettes.css';
  import Slider from '$shared/controls/Slider.svelte';
  import { iconSvg } from '$shared/design/icons.js';
  import { normalizePalette } from '$shared/design/themes.js';
  import { onMessage, post, getSession, getMacStatus, onMacStatus } from '../bridge.js';
  import { buildView, toggleChanges, perfView, sparkPoints, banners, ENGINES, engineLabel } from './panelModel.js';
  import { tick } from 'svelte';

  // Line icons the shared set does not have, drawn like the flyout's (flyout_icons.h): 16 grid, 1.5 stroke.
  const OWN = {
    settings: '<path d="M2 5h12M2 11h12M5 3v4M11 9v4"/>',
    quit: '<path d="M8 2v6M4.5 4.5a5 5 0 1 0 7 0"/>',
    chevdown: '<path d="M4.5 6.25l3.5 3.5 3.5-3.5"/>',
    check: '<path d="M3 8.5l3.2 3.2L13 4.5"/>',
    warn: '<path d="M8 1.75 14.75 13.75H1.25z"/><path d="M8 6v4M8 11.5v1"/>',
  };
  const icon = (name) => (OWN[name] ? `<svg class="ic" aria-hidden="true" focusable="false" viewBox="0 0 16 16">${OWN[name]}</svg>` : iconSvg(name));

  const init = window.__windInit || {};
  let values = $state({ ...(init.values || {}) });
  let mac = $state(init.mac || {});
  let perf = $state(init.perf || { level: 1, ms: [] });
  let open = $state(true);

  const palette = $derived(normalizePalette(values.uiPalette));
  const view = $derived(buildView(values));
  const pv = $derived(perfView(perf.level, perf.ms));
  const shownBanners = $derived(banners(mac));
  const engine = $derived(values.engine || 'zoom');

  getSession().then((s) => { values = { ...s.values }; });
  getMacStatus().then((m) => { const { type, ...rest } = m; mac = rest; });
  onMacStatus((m) => { const { type, ...rest } = m; mac = rest; });
  onMessage((m) => {
    if (!m) return;
    if (m.type === 'config') values = { ...m.values };
    else if (m.type === 'perf') perf = { level: m.level, ms: m.ms || [] };
    else if (m.type === 'panel') setOpen(!!m.open);
    // A refused write: the page set the value first, so ask the host for what Wind runs on (its config reply redraws).
    else if (m.type === 'configWriteFailed') post({ type: 'getConfig' });
  });

  // ---- writes
  function write(key, value) {
    values[key] = String(value);
    post({ type: 'setConfig', key, value: String(value) });
  }
  function flip(t) {
    const ch = toggleChanges(t.key, !t.on);
    Object.assign(values, ch);
    post({ type: 'setConfigs', values: ch });
  }
  function setEngine(v) {
    values.engine = v;
    post({ type: 'panelAction', action: 'setEngine', value: v });
  }
  const act = (action) => post({ type: 'panelAction', action });

  // ---- open, close, fade in (120 ms, as the flyout; none with reduced motion)
  let card = $state();
  let root = $state();
  const reduced = () => window.matchMedia && matchMedia('(prefers-reduced-motion: reduce)').matches;
  function setOpen(o) {
    open = o;
    if (!o) {
      listOpen = false;
      // A slider flushes its last value when it loses focus (Slider.svelte), before the host saves.
      if (document.activeElement && document.activeElement !== document.body) document.activeElement.blur();
      return;
    }
    if (card && !reduced() && card.animate) card.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 120, easing: 'cubic-bezier(.2,.7,.2,1)' });
  }

  // ---- the page's height for the host (the card, the engine list when it hangs below, the bottom margin)
  const EDGE = 14;
  let lastH = 0;
  function report() {
    if (!card) return;
    let b = card.getBoundingClientRect().bottom;
    const l = root?.querySelector('.elist');
    if (l) b = Math.max(b, l.getBoundingClientRect().bottom);
    const h = Math.ceil(b + EDGE);
    if (h !== lastH) { lastH = h; post({ type: 'panelSize', h }); }
  }
  $effect(() => {
    if (!card) return;
    const ro = new ResizeObserver(report);
    ro.observe(card);
    report();
    return () => ro.disconnect();
  });

  // ---- hover, for the e2e state (which control is under the real pointer)
  let hovered = '';
  function onOver(e) {
    const el = e.target instanceof Element ? e.target.closest('[data-id]') : null;
    const id = el ? el.dataset.id : '';
    if (id !== hovered) { hovered = id; post({ type: 'hover', id }); }
  }

  // ---- keys: Esc closes (the engine list first); Left/Right walk the toggle group
  function onKey(e) {
    if (e.key === 'Escape') {
      e.preventDefault();
      if (listOpen) { closeList(); return; }
      act('close');
      return;
    }
    const seg = e.target instanceof Element ? e.target.closest('.seg button') : null;
    if (seg && (e.key === 'ArrowLeft' || e.key === 'ArrowRight')) {
      e.preventDefault();
      const all = [...root.querySelectorAll('.seg button')];
      const i = all.indexOf(seg) + (e.key === 'ArrowRight' ? 1 : -1);
      all[Math.max(0, Math.min(all.length - 1, i))].focus();
    }
  }

  // ---- the engine picker: Wind's own list under the field (never macOS's menu over it), as wide as the field
  let listOpen = $state(false);
  let active = $state(0);
  let field = $state();
  let list = $state();
  let listTop = $state(0);
  let listLeft = $state(0);
  let listW = $state(0);
  async function openList() {
    if (listOpen) return;
    const r = field.getBoundingClientRect(), o = root.getBoundingClientRect();
    listTop = r.bottom - o.top + 4; listLeft = r.left - o.left; listW = r.width;
    active = Math.max(0, ENGINES.findIndex((x) => x.value === engine));
    listOpen = true;
    await tick();
    report();
    list?.focus({ preventScroll: true });
  }
  async function closeList(refocus = true) {
    if (!listOpen) return;
    listOpen = false;
    if (refocus) field?.focus({ preventScroll: true });
    await tick();
    report();
  }
  function choose(i) {
    const v = ENGINES[i].value;
    closeList();
    if (v !== engine) setEngine(v);
  }
  function onFieldKey(e) {
    if ([' ', 'Enter', 'ArrowDown', 'ArrowUp'].includes(e.key)) { e.preventDefault(); openList(); }
  }
  function onListKey(e) {
    const n = ENGINES.length;
    switch (e.key) {
      case 'ArrowDown': active = Math.min(n - 1, active + 1); break;
      case 'ArrowUp': active = Math.max(0, active - 1); break;
      case 'Home': active = 0; break;
      case 'End': active = n - 1; break;
      case 'Enter': case ' ': choose(active); break;
      case 'Escape': closeList(); break;
      case 'Tab': closeList(false); return;
      default: return;
    }
    e.preventDefault();
    e.stopPropagation();
  }
  $effect(() => {
    if (!listOpen) return;
    const down = (e) => { if (!list?.contains(e.target) && !field?.contains(e.target)) closeList(false); };
    window.addEventListener('pointerdown', down, true);
    return () => window.removeEventListener('pointerdown', down, true);
  });

  const BANNERS = {
    accessibility: { title: 'Wind needs Accessibility', text: 'Without it Wind cannot read your zoom keys', button: 'Open Privacy', action: 'openPrivacy' },
    zoomShortcuts: { title: 'macOS Zoom shortcuts are on', text: 'They clash with Wind’s zoom keys', button: 'Turn off', action: 'fixZoomShortcuts' },
  };
  const SPARK_W = 160, SPARK_H = 14;
</script>

<svelte:window onkeydown={onKey} />

<!-- svelte-ignore a11y_no_static_element_interactions -->
<div class="wnd panel" class:closed={!open} data-palette={palette} bind:this={root} onpointerover={onOver}
     onpointerleave={() => { if (hovered) { hovered = ''; post({ type: 'hover', id: '' }); } }}>
  <div class="card" bind:this={card} role="dialog" aria-label="Wind">
    {#each shownBanners as b (b)}
      <section class="banner" data-banner={b} data-id={'banner:' + b} role="alert">
        <span class="bic">{@html icon('warn')}</span>
        <div class="bt">
          <div class="bh">{BANNERS[b].title}</div>
          <div class="bd">{BANNERS[b].text}</div>
        </div>
        <button type="button" class="bfix" data-id={'fix:' + b} onclick={() => act(BANNERS[b].action)}>{BANNERS[b].button}</button>
      </section>
    {/each}

    {#if view.perf}
      <section class="head" data-id="perf" aria-label="Performance">
        <!-- Inline, as Banner.svelte: a url() in the stylesheet resolves against dist/assets/, not the page. -->
        <div class="aurora" aria-hidden="true" style="background-image:url(./c-grey.jpg)"></div>
        <div class="tint" aria-hidden="true"></div>
        <div class="scrim" aria-hidden="true"></div>
        <div class="top">
          <span class="zoom" class:idle={!pv.zoomed} data-zoom>{pv.zoom}</span>
          {#if pv.haveFps}<span class="fps" data-fps>{pv.fps} fps</span>{/if}
        </div>
        <div class="frame">
          <span class="fl">Frame</span>
          <svg class="spark" viewBox="0 0 {SPARK_W} {SPARK_H}" preserveAspectRatio="none" aria-hidden="true" focusable="false">
            {#if pv.spark.length >= 2}
              <polyline points={sparkPoints(pv.spark, SPARK_W, SPARK_H)} />
            {:else}
              <line class="dots" x1="0" y1={SPARK_H / 2} x2={SPARK_W} y2={SPARK_H / 2} />
            {/if}
          </svg>
          <span class="fm" data-ms>{pv.frameMs}</span>
        </div>
      </section>
    {/if}

    {#if view.sliders.length || view.toggles.length || view.engine}
      <section class="qs" class:ctl={view.toggles.length || view.engine}>
        {#each view.sliders as s (s.key)}
          <div class="srow" data-id={'slider:' + s.key} data-key={s.key} title={s.name}>
            <span class="sic">{@html icon(s.icon)}</span>
            <span class="sr-only" id={'lbl-' + s.key}>{s.name}</span>
            <Slider value={s.value} min={s.min} max={s.max} step={s.step} unit={s.unit} labelledby={'lbl-' + s.key}
                    onChange={(v) => write(s.key, v)} />
          </div>
        {/each}
        {#if view.toggles.length}
          <div class="seg" role="group" aria-label="Quick toggles">
            {#each view.toggles as t (t.key)}
              <button type="button" class:on={t.on} aria-pressed={t.on} aria-label={t.name} title={t.name}
                      data-id={'toggle:' + t.key} data-key={t.key} onclick={() => flip(t)}>{@html icon(t.icon)}</button>
            {/each}
          </div>
        {/if}
        {#if view.engine}
          <button type="button" class="engine" class:open={listOpen} bind:this={field} data-id="engine" data-key="engine"
                  aria-haspopup="listbox" aria-expanded={listOpen} aria-label={'Engine: ' + engineLabel(engine)} title="Engine"
                  onclick={() => (listOpen ? closeList() : openList())} onkeydown={onFieldKey}>
            <span class="eic">{@html icon('engine')}</span>
            <span class="ev">{engineLabel(engine)}</span>
            <span class="chev">{@html icon('chevdown')}</span>
          </button>
        {/if}
      </section>
    {/if}

    <section class="bar">
      <span class="brand" aria-hidden="true">{@html icon('logo')}<span>Wind</span></span>
      <button type="button" class="ib" data-id="settings" aria-label="Settings" title="Settings" onclick={() => act('openSettings')}>{@html icon('settings')}</button>
      <button type="button" class="ib" data-id="quit" aria-label="Quit Wind" title="Quit Wind" onclick={() => act('quit')}>{@html icon('quit')}</button>
    </section>
  </div>

  {#if listOpen}
    <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
    <ul class="elist" role="listbox" tabindex="-1" aria-label="Engine" bind:this={list} aria-activedescendant={'eng-' + active}
        style="top:{listTop}px; left:{listLeft}px; width:{listW}px" onkeydown={onListKey}>
      <li class="cap" role="presentation">Engine</li>
      {#each ENGINES as e, i (e.value)}
        <!-- svelte-ignore a11y_click_events_have_key_events -->
        <li id={'eng-' + i} role="option" aria-selected={e.value === engine} class:active={i === active} class:cur={e.value === engine}
            data-id={'engine:' + e.value} onpointerenter={() => (active = i)} onclick={() => choose(i)}>
          <span>{e.label}</span>
          {#if e.value === engine}<span class="ck">{@html icon('check')}</span>{/if}
        </li>
      {/each}
    </ul>
  {/if}
</div>

<style>
  /* The window is clear: 14 px of it around the card (none above, macOS keeps windows out of the menu
     bar) hold the CSS shadow. Geometry in px from flyout_model.h: width 300, 1 px border, rows 40,
     padding 20, icon 16 + 12, value 48, controls 32, bottom bar 40 with 32 px buttons. */
  .panel { position: relative; box-sizing: border-box; width: 328px; padding: 0 14px 14px; background: transparent;
           font: 12px var(--m); color: var(--f-fg); -webkit-font-smoothing: antialiased; }
  .card { position: relative; width: 300px; box-sizing: border-box; overflow: hidden; border: 1px solid var(--f-menub);
          border-radius: var(--rc); background: var(--f-menu); box-shadow: 0 6px 14px rgba(0, 0, 0, .45);
          display: flex; flex-direction: column; }
  /* 1 px rules between the sections (flyout hairlines), never above the first. */
  section + section { border-top: 1px solid var(--f-rule); }

  .panel :global(svg.ic) { width: 16px; height: 16px; fill: none; stroke: currentColor; stroke-width: 1.5;
                           stroke-linecap: round; stroke-linejoin: round; display: block; }

  /* ---- banners (Mac only) */
  /* Icon beside the text, the fix button under it: 300 px leaves no room for a button beside two lines. */
  .banner { display: grid; grid-template-columns: 16px 1fr; column-gap: 12px; padding: 14px 20px;
            background: color-mix(in srgb, var(--danger) 9%, var(--f-menu)); }
  .bic { color: var(--danger); margin-top: 1px; }
  .bfix { grid-column: 2; justify-self: start; margin-top: 10px; }
  .bh { font: 600 12.5px var(--s); color: var(--f-fg); }
  .bd { margin-top: 2px; font: 11.5px var(--s); color: var(--f-fg2); text-wrap: pretty; }
  .bfix { height: 26px; padding: 0 10px; border: 0; border-radius: var(--rs); background: var(--dangerbtn);
          color: var(--dangerbtnfg); font: 600 12px var(--s); cursor: pointer; white-space: nowrap;
          transition: filter 120ms var(--ease), transform 80ms var(--ease); }
  .bfix:hover { filter: brightness(1.1); }
  .bfix:active { transform: scale(.97); }

  /* ---- Performance header (96 px): the aurora image faded in from the right, two scrims, the level and
     fps, then Frame, the frame-time trace and the mean */
  .head { position: relative; height: 96px; box-sizing: border-box; padding: 18px 20px 0; background: var(--f-band);
          isolation: isolate; overflow: hidden; }
  .aurora { position: absolute; inset: 0; background: right center / cover no-repeat;
            opacity: var(--f-aurora); -webkit-mask-image: linear-gradient(90deg, transparent 15%, #000 70%);
            mask-image: linear-gradient(90deg, transparent 15%, #000 70%); z-index: -3; }
  .tint { position: absolute; inset: 0; background: var(--f-tint); opacity: var(--f-tinta); mix-blend-mode: color; z-index: -2; }
  .scrim { position: absolute; inset: 0; z-index: -1;
           background: linear-gradient(180deg, transparent 40%, var(--f-scrim)), linear-gradient(90deg, var(--f-scrim), transparent 85%); }
  .top { display: flex; align-items: center; justify-content: space-between; height: 28px; }
  .zoom { font: 500 28px/28px var(--s); letter-spacing: -.7px; color: var(--f-fg); font-variant-numeric: tabular-nums; }
  .zoom.idle { color: var(--f-fg3); }
  .fps { font: 600 12px var(--m); color: var(--f-fg); font-variant-numeric: tabular-nums; }
  .frame { display: flex; align-items: center; gap: 10px; height: 14px; margin-top: 14px; }
  .fl { color: var(--f-fg2); }
  .spark { flex: 1; min-width: 8px; height: 14px; overflow: visible; }
  .spark polyline { fill: none; stroke: var(--f-spark); stroke-width: 1.5; stroke-linejoin: round; vector-effect: non-scaling-stroke; }
  .spark .dots { stroke: color-mix(in srgb, var(--f-fg3) 55%, transparent); stroke-width: 1; stroke-dasharray: 1 3; vector-effect: non-scaling-stroke; }
  .fm { font-weight: 600; color: var(--f-fg); font-variant-numeric: tabular-nums; }

  /* ---- quick controls on the lifted band: slider rows, then the toggle group and the engine field */
  .qs { background: var(--f-lift); padding: 8px 0; display: flex; flex-direction: column;
        --fill: var(--f-fill); --track: var(--f-track); --thumb: var(--f-fg); --fg: var(--f-fg); --focus: var(--f-focus); }
  .qs.ctl { padding-bottom: 14px; }
  .srow { display: flex; align-items: center; height: 40px; padding: 0 20px; gap: 12px; color: var(--f-glyph); }
  .sic { flex: none; }
  .srow :global(.sl) { flex: 1; min-width: 0; gap: 0; }
  .srow :global(.sl input) { width: auto; flex: 1 1 auto; height: 24px; margin: 0 12px 0 0; }
  .srow :global(.sl input::-webkit-slider-runnable-track) { height: 6px; border-radius: 3px; }
  .srow :global(.sl input::-webkit-slider-thumb) { width: 3px; height: 10px; margin-top: -2px; border-radius: 1.5px; }
  .srow :global(.sl .val) { width: 48px; margin: 0; font: 12px var(--m); color: var(--f-fg); }
  .srow :global(.sl input:focus-visible) { outline: 2px solid var(--f-focus); outline-offset: 4px; border-radius: var(--rs); }

  .seg { position: relative; display: flex; gap: 1px; height: 32px; margin: 6px 20px 0; border-radius: var(--rk);
         overflow: hidden; background: var(--f-segline); }
  .seg::after { content: ''; position: absolute; inset: 0; border-radius: inherit; box-shadow: inset 0 0 0 1px var(--f-ring); pointer-events: none; }
  .seg button { position: relative; flex: 1; display: grid; place-items: center; border: 0; padding: 0; margin: 0;
                background: var(--f-off); color: var(--f-offic); cursor: pointer;
                transition: background-color 150ms var(--ease), color 150ms var(--ease); }
  .seg button::before { content: ''; position: absolute; inset: 0; background: var(--f-offh); opacity: 0; transition: opacity 120ms var(--ease); }
  .seg button :global(svg) { position: relative; stroke-width: 1.75; }
  .seg button:hover::before { opacity: 1; }
  .seg button:hover { color: var(--f-fg); }
  .seg button.on { background: var(--f-on); color: var(--f-onic); }
  .seg button.on::before { background: var(--f-onh); }
  .seg button:focus-visible { outline: none; box-shadow: inset 0 0 0 2px var(--f-focus); }
  .seg button.on:focus-visible { box-shadow: inset 0 0 0 2px var(--f-onic); }

  .engine { position: relative; display: flex; align-items: center; height: 32px; margin: 8px 20px 0; padding: 0 10px;
            border: 0; border-radius: var(--rk); background: var(--f-off); color: var(--f-fg); font: 12px var(--m);
            box-shadow: inset 0 0 0 1px var(--f-ring); cursor: pointer; text-align: left;
            transition: background-color 120ms var(--ease), box-shadow 120ms var(--ease); }
  .seg + .engine { margin-top: 8px; }
  .qs > .engine:first-child, .srow + .engine { margin-top: 6px; }
  .engine:hover { background: var(--f-offh); }
  .engine.open { box-shadow: inset 0 0 0 1px var(--f-onb); }
  .engine:focus-visible { outline: none; box-shadow: inset 0 0 0 2px var(--f-focus); }
  .eic { color: var(--f-glyph); margin-right: 10px; }
  .ev { flex: 1; }
  .chev { color: var(--f-offic); transition: transform 150ms var(--ease); }
  .chev :global(svg) { stroke-width: 1.75; }
  .engine.open .chev { transform: rotate(180deg); }

  /* ---- bottom bar: the brand, then Settings and Quit (32 px, 4 apart, 12 from the edge) */
  .bar { display: flex; align-items: center; gap: 4px; height: 40px; padding: 0 12px 8px 20px; box-sizing: content-box; }
  .brand { flex: 1; display: flex; align-items: center; gap: 8px; color: var(--f-fg3); }
  .ib { width: 32px; height: 32px; display: grid; place-items: center; border: 0; padding: 0; border-radius: var(--rk);
        background: transparent; color: var(--f-glyph); cursor: pointer;
        transition: background-color 120ms var(--ease), color 120ms var(--ease), transform 80ms var(--ease); }
  .ib:hover { background: var(--f-hl); color: var(--f-fg); }
  .ib:active { transform: scale(.97); }
  .ib:focus-visible { outline: 2px solid var(--f-focus); outline-offset: -2px; }

  /* ---- the engine list (flyout_model.h ComputeList: 4 px padding, a 28 px caption, 30 px rows) */
  .elist { position: absolute; z-index: 10; margin: 0; padding: 4px; list-style: none; box-sizing: border-box;
           border: 1px solid var(--f-menub); border-radius: var(--rl); background: var(--f-card);
           box-shadow: 0 6px 14px rgba(0, 0, 0, .45); outline: none; animation: drop 120ms var(--ease); }
  .elist li { display: flex; align-items: center; height: 30px; padding: 0 8px 0 10px; border-radius: var(--rs);
              color: var(--f-fg2); cursor: pointer; }
  .elist li.cap { height: 28px; color: var(--f-fg3); cursor: default; }
  .elist li span:first-child { flex: 1; }
  .elist li.active { background: var(--f-hl); color: var(--f-fg); }
  .elist li.cur { color: var(--f-fg); }
  .ck { color: var(--f-spark); }
  .ck :global(svg) { stroke-width: 1.75; }
  @keyframes drop { from { opacity: 0; transform: translateY(-4px); } }

  @media (prefers-reduced-motion: reduce) {
    .panel *, .panel *::before { transition-duration: .01ms !important; animation: none !important; }
  }
</style>
