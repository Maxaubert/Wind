<!-- Copyright (c) 2026 Max Aubert. All rights reserved.
     Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary -->
<script>
  // Settings window, Wind for Mac: every change applies at once and is final (no profiles, no Save or
  // Discard). `values` is what config.json holds, as the host reports it (strings). Each change goes
  // to the host with setConfig(key, value); the host types it, clamps it and saves config.json, and
  // Wind's file watcher applies it live. Edits made elsewhere (the menu bar, a text editor) arrive as
  // a pushed config ({type:'config', push:true}) and refresh the page.
  import { onMount, tick } from 'svelte';
  import './design/tokens.css';
  import './design/themes.css';
  import { groups, missingPermissions } from './settings-schema.js';
  import { getSession, setConfig, openIni, exportDiagnostics, windowControl, onMessage,
           getMacStatus, onMacStatus, macAction } from './bridge.js';
  import { fill } from './session.js';
  import { themes, normalizePalette } from './design/themes.js';
  import { capturing } from './lib/capture.js';
  import Sidebar from './shell/Sidebar.svelte';
  import ScrollChip from './shell/ScrollChip.svelte';
  import Banner from './shell/Banner.svelte';
  import PermissionBanner from './shell/PermissionBanner.svelte';
  import Card from './shell/Card.svelte';
  import SettingRow from './controls/SettingRow.svelte';
  import Prompt from './prompts/Prompt.svelte';
  import Results from './search/Results.svelte';
  import { search } from './search/search.js';

  // Replaced at build time with ../VERSION (vite.config.js); empty in tests.
  const VERSION = typeof __WIND_VERSION__ === 'string' ? __WIND_VERSION__ : '';

  let values = $state({});
  let mac = $state(null);              // the latest macStatus (permissions, macOS Zoom shortcuts, login item)
  let activeId = $state('hotkeys');   // Settings opens on Hotkeys
  let loaded = $state(false);
  let main = $state();

  const top = groups.filter((g) => !g.bottom);
  const bottom = groups.filter((g) => g.bottom);   // below the sidebar divider
  const group = $derived(groups.find((g) => g.id === activeId) || groups[0]);

  // The built-in theme (uiPalette) picks the token block in design/themes.css. Always dark.
  // Until the config is loaded, use the palette the host injected for the first paint (window.__windInit).
  const initPalette = normalizePalette(window.__windInit && window.__windInit.values && window.__windInit.values.uiPalette);
  const palette = $derived(loaded ? normalizePalette(values.uiPalette) : initPalette);
  const missing = $derived(missingPermissions(mac, values.engine));

  // --- Screen-reader announcements ------------------------------------------------------------
  let announcement = $state('');
  function announce(msg) { announcement = announcement === msg ? msg + '​' : msg; }

  // --- Changes --------------------------------------------------------------------------------
  // Keys this page wrote in the last moments: a config pushed meanwhile (a menu edit) must not drag a
  // slider back under the hand to a value it already left.
  const wroteAt = new Map();
  const HOLD_MS = 600;
  function write(key, val) {
    wroteAt.set(key, performance.now());
    setConfig(key, val);
  }
  function change(key, val) {
    if (key === 'engine') announce('Engine set to ' + (val === 'zoom' ? 'macOS zoom' : 'Wind draws') + '. Some options changed.');
    if (key === 'uiPalette') announce('Theme ' + (themes.find((t) => t.id === val)?.label ?? val));
    values = { ...values, [key]: String(val) };
    write(key, val);
  }
  // Keybind captures and the pan row: a patch of one or more keys, written at once.
  function live(patch) {
    for (const k of Object.keys(patch)) write(k, patch[k]);
    values = { ...values, ...Object.fromEntries(Object.entries(patch).map(([k, v]) => [k, String(v)])) };
  }
  function apply(session) {
    const next = fill(session);
    const now = performance.now();
    for (const [k, t] of wroteAt) {
      if (now - t < HOLD_MS) { if (k in values) next[k] = values[k]; } else wroteAt.delete(k);
    }
    if (JSON.stringify(next) !== JSON.stringify(values)) values = next;
  }
  // A refused or failed write: the page shows a value config.json never took, so read it again.
  async function refresh() {
    if (!loaded) return;
    wroteAt.clear();
    apply(await getSession());
  }
  let writeError = $state('');
  onMount(() => {
    const offs = [
      onMessage((m) => { if (m && m.type === 'configWriteFailed') { writeError = m.key || 'a setting'; refresh(); } }),
      onMessage((m) => { if (m && m.type === 'config' && m.push && loaded) apply(m); }),
      onMacStatus((m) => { mac = m; }),
    ];
    (async () => {
      apply(await getSession());
      loaded = true;
      getMacStatus().then((m) => { mac = m; });
      // Start on the page: focus the page region, named after its heading, unless the user already put focus somewhere.
      await tick();
      if (main && (!document.activeElement || document.activeElement === document.body)) main.focus({ preventScroll: true });
    })();
    // Coming back to the window (from System Settings, say): ask for the macOS status again.
    const onFocus = () => { if (loaded) getMacStatus().then((m) => { mac = m; }); };
    window.addEventListener('focus', onFocus);
    return () => { offs.forEach((o) => o()); window.removeEventListener('focus', onFocus); };
  });

  // --- Search ---------------------------------------------------------------------------------
  let query = $state('');
  const results = $derived(search(groups, query));
  const searching = $derived(query.trim() !== '');
  const searchInput = () => document.querySelector('.side .search input');
  function onSearch(q) { query = q; }
  function clearSearch() { query = ''; const i = searchInput(); if (i) i.value = ''; }
  // A result's control is the real one; a row hidden on its page by showIf (it only applies under another
  // setting) shows dimmed, like the rest of the app shows a gated control.
  const isDisabled = (r) => !visible(r) || isOff(r);
  // Enter or Down in the search box moves into the best result's control (Tab then walks the rest).
  function focusFirstResult() {
    const c = main && main.querySelector('.res .hit button, .res .hit input, .res .hit select, .res .hit [tabindex="0"]');
    if (c) c.focus();
  }
  // Mac shortcuts: Cmd+F searches, Cmd+Q quits Wind, Escape clears the search.
  function onKeydown(e) {
    if (capturing()) return;   // the keys belong to the binding being recorded
    const cmd = e.metaKey && !e.ctrlKey && !e.altKey && !e.shiftKey;
    if (cmd && e.key.toLowerCase() === 'f') { e.preventDefault(); const i = searchInput(); if (i) { i.focus(); i.select(); } return; }
    if (e.key === 'Escape' && (searching || e.target === searchInput())) {
      e.preventDefault(); clearSearch(); if (main) main.focus({ preventScroll: true }); return;
    }
    if ((e.key === 'Enter' || e.key === 'ArrowDown') && e.target === searchInput() && results.length) { e.preventDefault(); focusFirstResult(); return; }
    if (cmd && e.key.toLowerCase() === 'q') { e.preventDefault(); windowControl('quitWind'); }
  }

  // --- Navigation -----------------------------------------------------------------------------
  let navigated = $state(false);   // the page cross-fade stays off until the first navigation (none on first paint)
  async function select(id) {
    navigated = true;
    activeId = id;
    await tick();
    if (main) { main.scrollTop = 0; main.focus({ preventScroll: true }); }
  }
  function onAction(a) {
    if (a === 'openIni') openIni();
    else if (a === 'exportDiagnostics') { exportDiagnostics(); announce('Exporting diagnostics to the Desktop'); }
    else if (a === 'quitWind') windowControl('quitWind');
    else macAction(a);   // showLog, rerunSetup
  }
  function onMacAction(a, v = '') {
    macAction(a, v);
    if (a === 'setLogin') mac = { ...(mac || {}), loginItem: Number(v) === 1 };   // the host pushes the real state after
  }
  // offIf: the row stays in place but dimmed and inert while another setting has that value.
  const isOff = (r) => !!r.offIf && String(values[r.offIf.key]) === String(r.offIf.eq);
  const visible = (r) => !r.showIf || (r.showIf.ne !== undefined ? String(values[r.showIf.key] ?? '') !== String(r.showIf.ne) : String(values[r.showIf.key]) === String(r.showIf.eq));
  // Advanced rows show on their page only while the switch is on; search results show them always.
  const advOn = $derived(Number(values.showAdvanced) === 1);
  const shown = (r) => visible(r) && (!r.adv || advOn);
  const extra = $derived({ version: VERSION, mac, onAction, onMacAction });
</script>

<svelte:window onkeydown={onKeydown} />

<div class="wnd app" class:pending={!loaded} data-palette={palette}>
  <div class="body">
    <Sidebar groups={top} {bottom} active={searching ? '' : activeId} version={VERSION} {query} {onSearch}
             onSelect={(id) => { clearSearch(); select(id); }} />
    <main class="main" data-page={searching ? 'search' : group.id} bind:this={main} tabindex="-1" aria-label={searching ? 'Search results' : group.label}>
      {#key searching ? '?search' : activeId}
      <div class="page" class:fade={navigated}>
      {#if searching}
        <PermissionBanner {missing} onFix={(a) => macAction(a)} />
        <Results {results} {query} {values} {extra} {live} onChange={change} onSet={change} {isDisabled} />
      {:else}
      <Banner title={group.label} description={group.desc} icon={group.icon} />
      <PermissionBanner {missing} onFix={(a) => macAction(a)} />
      {#each group.cards as card, i (group.id + i)}
        {@const rows = card.rows.filter(shown)}
        {#if rows.length}
        <Card caption={card.caption}>
          {#each rows as r (r.key)}
            <SettingRow row={r} value={values[r.key]} {values} {extra} {live} disabled={isOff(r)}
                        onChange={(v) => change(r.key, v)} onSet={change} />
          {/each}
        </Card>
        {/if}
      {/each}
      {/if}
      </div>
      {/key}
      <div class="tail"></div>
    </main>
    <ScrollChip target={main} />
  </div>
  <div class="sr-only" role="status" aria-live="polite" aria-atomic="true">{announcement}</div>

  {#if writeError}
    <Prompt id="wr" title="Couldn't save the setting"
            text={'Wind could not write "' + writeError + '" to its settings file, so the change did not stick. Try again in a moment.'}
            onEsc={() => (writeError = '')}
            buttons={[{ label: 'Close', kind: 'primary', onClick: () => (writeError = '') }]} />
  {/if}
</div>

<style>
  /* Hidden until the config is loaded, so no empty controls flash; the host paints the window background behind it. */
  .app.pending { visibility: hidden; }
  /* No title bar row: macOS draws its traffic lights over the top-left of the sidebar (Sidebar.svelte keeps that free). */
  .app { --side-w: 240px; width: 100vw; height: 100vh; display: grid; grid-template-rows: 1fr; position: relative; overflow: hidden; }
  .body { display: grid; grid-template-columns: var(--side-w) minmax(0, 1fr); min-height: 0; }
  /* Narrow window: the sidebar moves above the page as a wrapped strip instead of eating the content. */
  @media (max-width: 700px) {
    .app { --side-w: 0px; }
    .body { grid-template-columns: minmax(0, 1fr); grid-template-rows: auto minmax(0, 1fr); }
  }
  .body { position: relative; }   /* the scroll chip sits on the right edge of the page column */
  /* No native scrollbar on the page: ScrollChip shows the position instead. */
  /* The page is a size container: --gx is the side gutter (smaller when narrow) and caps the content at 960px on wide windows. */
  .main { position: relative; min-height: 0; min-width: 0; overflow-y: auto; overflow-x: hidden; padding: 0; outline: none; scrollbar-width: none;
          container-type: inline-size; }
  .main::-webkit-scrollbar { display: none; width: 0; height: 0; }
  .page, .tail { --gx: max(clamp(16px, 4cqw, 40px), calc((100cqw - 960px) / 2)); padding-inline: var(--gx); }
  .page.fade { animation: pagein var(--dur-fast) var(--ease); }
  @keyframes pagein { from { opacity: 0; } }
  .tail { height: 48px; padding: 0; }
  .sr-only { position: absolute; width: 1px; height: 1px; margin: -1px; padding: 0; overflow: hidden;
             clip: rect(0 0 0 0); white-space: nowrap; border: 0; }
</style>
