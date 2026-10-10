<script>
  // Settings window: instant apply with an explicit Save (spec 2026-10-01).
  //   values  the session: what the live ini holds right now. Every change is written at once
  //           (setConfig), so it takes effect immediately.
  //   saved   what the active profile file holds. The capsule counts the keys where the two differ.
  // Keybind captures are the exception: they write the live ini AND the profile (setConfigPersist),
  // so they are never "unsaved" and a later Save cannot lose them.
  import { onMount, tick } from 'svelte';
  import './design/tokens.css';
  import './design/themes.css';
  import { groups } from './settings-schema.js';
  import { getSession, setConfig, setConfigPersist, saveSession, discardSession, openIni,
           exportDiagnostics, openRepo, pickExe, windowControl, onMessage,
           setDirty, switchProfile, createProfile, deleteProfile } from './bridge.js';
  import { fill, changedKeys } from './session.js';
  import { themes, normalizePalette } from './design/themes.js';
  import { droppedBinds } from './lib/keybindRules.js';
  import TitleBar from './shell/TitleBar.svelte';
  import Sidebar from './shell/Sidebar.svelte';
  import ScrollChip from './shell/ScrollChip.svelte';
  import Banner from './shell/Banner.svelte';
  import Card from './shell/Card.svelte';
  import SaveCapsule from './shell/SaveCapsule.svelte';
  import SettingRow from './controls/SettingRow.svelte';
  import Prompt from './prompts/Prompt.svelte';
  import Results from './search/Results.svelte';
  import NewProfileDialog from './prefs/NewProfileDialog.svelte';
  import TrayMenuPage from './tray/TrayMenuPage.svelte';
  import { search } from './search/search.js';

  // Replaced at build time with src/version.h's WIND_VERSION_STR (vite.config.js); empty in tests.
  const VERSION = typeof __WIND_VERSION__ === 'string' ? __WIND_VERSION__ : '';

  let values = $state({});
  let saved = $state({});
  let prof = $state({ names: [], active: '' });
  let activeId = $state('hotkeys');   // Settings opens on Hotkeys
  let loaded = $state(false);
  let main = $state();
  let maximized = $state(false);

  const top = groups.filter((g) => !g.bottom);
  const bottom = groups.filter((g) => g.bottom);   // below the sidebar divider
  const group = $derived(groups.find((g) => g.id === activeId) || groups[0]);
  const count = $derived(loaded ? changedKeys(values, saved).length : 0);
  const dirty = $derived(count > 0);
  $effect(() => { if (loaded) setDirty(dirty); });   // the host's WM_CLOSE guard follows the UI

  // The built-in theme (uiPalette, a global key) picks the token block in design/themes.css. Always dark (#324).
  // Until the session is loaded, use the palette the host injected with the first-paint config (window.__windInit),
  // so the very first frame already has the user's theme instead of Wind grey's tokens.
  const initPalette = normalizePalette(window.__windInit && window.__windInit.values && window.__windInit.values.uiPalette);
  const palette = $derived(loaded ? normalizePalette(values.uiPalette) : initPalette);

  // --- Screen-reader announcements ------------------------------------------------------------
  let announcement = $state('');
  function announce(msg) { announcement = announcement === msg ? msg + '​' : msg; }

  // --- Loading --------------------------------------------------------------------------------
  let dropped = $state([]);
  let runningModel = $state('');   // the engine the live Wind process runs (read once at launch)
  async function load(session) {
    const s = session || await getSession();
    const f = fill(s);
    // Binds from an older version that the safety rules now refuse (#285): reset once, say why.
    // Binds are deliberate, so the reset persists like any other keybind change.
    const found = droppedBinds(f.values);
    if (found.length) {
      for (const d of found) for (const k of d.keys) { f.values[k] = '0'; f.saved[k] = '0'; setConfigPersist(k, '0'); }
      dropped = found;
    }
    values = f.values; saved = f.saved;
    if (s.profiles) prof = { names: s.profiles.names || [], active: s.profiles.active || '' };
    // The host reports the engine Wind really runs; the ini only says what it will run next start.
    if (!runningModel) runningModel = s.runningModel || String(f.values.model);
    loaded = true;
  }

  // --- Changes --------------------------------------------------------------------------------
  // High resolution cursor notice (#441): turning it ON asks first, unless the user opted out
  // (uiHighResNoticeOff, a global UI-only key). Every row that renders the key (page and search) comes here.
  let highResPrompt = $state(false);
  let highResSkip = $state(false);
  function acceptHighRes() {
    if (highResSkip) { values = { ...values, uiHighResNoticeOff: '1' }; setConfig('uiHighResNoticeOff', '1'); }
    highResPrompt = false; highResSkip = false;
    values = { ...values, txSamplingMode: 1 };
    setConfig('txSamplingMode', 1);
  }
  function cancelHighRes() { highResPrompt = false; highResSkip = false; }
  function change(key, val) {
    if (key === 'txSamplingMode' && Number(val) === 1 && Number(values.txSamplingMode) !== 1 &&
        String(values.uiHighResNoticeOff) !== '1') { highResSkip = false; highResPrompt = true; return; }
    if (key === 'model') announce('Magnifier model set to ' + val + '. Some display options changed.');
    if (key === 'uiPalette') announce('Theme ' + (themes.find((t) => t.id === val)?.label ?? val));
    values = { ...values, [key]: val };
    setConfig(key, val);
  }
  // Keybind captures: live AND saved at once, so the hook stops swallowing the old binding and a
  // later Save or Discard cannot lose them.
  function live(patch) {
    for (const k of Object.keys(patch)) setConfigPersist(k, patch[k]);
    values = { ...values, ...patch };
    saved = { ...saved, ...patch };
  }
  // The ini is shared with the tray flyout and hand edits, so the page can fall behind it. Re-read it
  // when the window regains focus (that is when the user comes back from the flyout or an editor),
  // and after a failed write (the page then shows a value the ini never took). Only a real
  // difference touches the page, so nothing flickers.
  async function refresh() {
    if (!loaded) return;
    const s = await getSession();
    const f = fill(s);
    if (s.profiles) prof = { names: s.profiles.names || [], active: s.profiles.active || '' };
    if (JSON.stringify(f.values) === JSON.stringify(values) && JSON.stringify(f.saved) === JSON.stringify(saved)) return;
    await load(s);
  }
  let restartError = $state(false);
  let writeError = $state('');
  let saveError = $state(false);
  onMount(() => {
    const offs = [
      onMessage((m) => { if (m && m.type === 'configWriteFailed') { writeError = m.key || 'a setting'; refresh(); } }),
      // Maximized: no window outline (the host reports the state on every resize).
      onMessage((m) => { if (m && m.type === 'windowState') { maximized = !!m.maximized; document.documentElement.toggleAttribute('data-maximized', maximized); } }),
      onMessage((m) => {
        if (m && m.type === 'restartFailed') {
          values = { ...values, model: runningModel }; setConfig('model', runningModel);
          restartError = true;
        }
      }),
      // The tray switched profiles under us: the host already rewrote the live ini, so reload.
      onMessage(async (m) => {
        if (!m || m.type !== 'profiles' || !m.push) return;
        const changed = m.active !== prof.active;
        prof = { names: m.names, active: m.active };
        if (changed) { await load(); announce('Profile switched to ' + m.active + '. Settings reloaded.'); }
      }),
      onMessage((m) => { if (m && m.type === 'confirmClose' && dirty) closePrompt = true; }),
    ];
    (async () => {
      await load();
    })();
    window.addEventListener('focus', refresh);
    return () => { offs.forEach((o) => o()); window.removeEventListener('focus', refresh); };
  });

  // --- Save / Discard -------------------------------------------------------------------------
  async function save() {
    const ok = await saveSession();
    if (!ok) { saveError = true; return false; }
    saved = { ...values };
    announce('Settings saved');
    return true;
  }
  async function discard() {
    const s = await discardSession();
    await load(s);
    announce('Changes discarded');
  }

  // --- Profiles -------------------------------------------------------------------------------
  // Switching, creating from the defaults and deleting the ACTIVE profile replace the session, so with
  // unsaved changes they ask first (Save / Discard / Cancel). Deleting another profile does not, and neither
  // does a new profile that starts from the current settings: the unsaved changes carry into it.
  let profilePrompt = $state(null);   // pending { kind, payload }
  let profileError = $state('');
  let newDialog = $state(false);      // the New profile dialog
  let deleteTarget = $state('');      // the profile the delete confirmation is about ('' = closed)
  function profileAction(kind, payload) {
    const deletesActive = kind === 'delete' && payload.name.toLowerCase() === prof.active.toLowerCase();
    const replaces = kind === 'switch' || (kind === 'create' && payload.from !== 'current') || deletesActive;
    if (replaces && dirty) { profilePrompt = { kind, payload }; return; }
    runProfileAction(kind, payload);
  }
  async function resolveProfilePrompt(how) {
    const p = profilePrompt; profilePrompt = null;
    if (how === 'save') { if (!(await save())) return; }
    else await discard();
    runProfileAction(p.kind, p.payload);
  }
  async function runProfileAction(kind, payload) {
    profileError = '';
    const prevActive = prof.active;
    let r;
    if (kind === 'switch') r = await switchProfile(payload.name);
    else if (kind === 'create') r = await createProfile(payload.name, payload.from === 'current');
    else if (kind === 'delete') r = await deleteProfile(payload.name);
    else return;
    prof = { names: r.names, active: r.active };
    // Reload BEFORE the error check: a failed op can still have rewritten the live ini.
    if (kind === 'switch' || kind === 'create' || (kind === 'delete' && r.active !== prevActive)) await load();
    if (!r.ok) { profileError = r.error || 'Profile operation failed'; return; }
    if (kind === 'switch') announce('Switched to profile ' + payload.name + '. Settings reloaded.');
    else if (kind === 'create' && payload.from === 'current') {
      // The host built the new profile from the live session (unsaved changes included), so the
      // reload above already shows them as saved: nothing to write back from here.
      announce('Created profile ' + payload.name + ' from the current settings.');
    }
    else if (kind === 'create') announce('Created profile ' + payload.name + ' with the default settings.');
    else if (kind === 'delete') announce('Deleted profile ' + payload.name);
  }

  // --- Close and quit -------------------------------------------------------------------------
  let closePrompt = $state(false);
  let quitPrompt = $state(false);
  function requestClose() { if (dirty) closePrompt = true; else windowControl('close'); }
  function requestQuit() { if (dirty) quitPrompt = true; else doQuit(); }
  function doQuit() { setDirty(false); windowControl('quitWind'); }
  async function closeWith(how) {
    closePrompt = false;
    if (how === 'save') { if (!(await save())) return; }
    else if (how === 'discard') await discard();
    windowControl('close', true);   // 'keep' leaves the session live until Wind quits
  }
  async function quitWith(how) {
    quitPrompt = false;
    if (how === 'save') { if (!(await save())) return; }
    else await discard();
    doQuit();
  }
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
  function onKeydown(e) {
    const mod = e.ctrlKey && !e.altKey && !e.shiftKey;
    if (mod && e.key.toLowerCase() === 'f') { e.preventDefault(); const i = searchInput(); if (i) { i.focus(); i.select(); } return; }
    if (e.key === 'Escape' && (searching || e.target === searchInput())) {
      e.preventDefault(); clearSearch(); if (main) main.focus({ preventScroll: true }); return;
    }
    if ((e.key === 'Enter' || e.key === 'ArrowDown') && e.target === searchInput() && results.length) { e.preventDefault(); focusFirstResult(); return; }
    if (mod && e.key.toLowerCase() === 'q') { e.preventDefault(); requestQuit(); }
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
    else if (a === 'exportDiagnostics') exportDiagnostics();
    else if (a === 'quitWind') requestQuit();
  }
  // offIf: the row stays in place but dimmed and inert while another setting has that value (#427).
  const isOff = (r) => !!r.offIf && String(values[r.offIf.key]) === String(r.offIf.eq);
  const visible = (r) => !r.showIf || (r.showIf.ne !== undefined ? String(values[r.showIf.key]) !== String(r.showIf.ne) : String(values[r.showIf.key]) === String(r.showIf.eq));
  // Advanced rows show on their page only while the global switch is on; search results show them always.
  const advOn = $derived(Number(values.showAdvanced) === 1);
  const shown = (r) => visible(r) && (!r.adv || advOn);
  const extra = $derived({
    runningModel, version: VERSION, onRepo: openRepo,
    onAction, pick: pickExe,
    onRestart: () => { restartError = false; windowControl('restartWind'); },
    profiles: {
      names: prof.names, active: prof.active,
      onSwitch: (name) => profileAction('switch', { name }),
      onNew: () => (newDialog = true),
      onDelete: (name) => (deleteTarget = name),
    },
  });
</script>

<svelte:window onkeydown={onKeydown} />

<div class="wnd app" class:pending={!loaded} data-palette={palette}>
  <TitleBar {maximized} onMinimize={() => windowControl('minimize')} onMaximize={() => windowControl('maximize')} onClose={requestClose} />
  <div class="body">
    <Sidebar groups={top} {bottom} active={searching ? '' : activeId} version={VERSION} {query} {onSearch}
             onSelect={(id) => { clearSearch(); select(id); }} />
    <main class="main" data-page={searching ? 'search' : group.id} bind:this={main} tabindex="-1" aria-label={group.label}>
      {#key searching ? '?search' : activeId}
      <div class="page" class:fade={navigated}>
      {#if searching}
        <Results {results} {query} {values} {extra} {live} onChange={change} onSet={change} {isDisabled} />
      {:else}
      <Banner title={group.label} description={group.desc} icon={group.icon} />
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
      {#if group.custom === 'tray'}
        <TrayMenuPage {values} onChange={change} {announce} />
      {/if}
      {/if}
      </div>
      {/key}
      <div class="tail"></div>
    </main>
    <ScrollChip target={main} />
  </div>
  <SaveCapsule {count} onSave={save} onDiscard={discard} />
  <div class="sr-only" role="status" aria-live="polite" aria-atomic="true">{announcement}</div>

  {#if highResPrompt}
    <Prompt id="hires" title="High resolution cursor is experimental"
            text="It makes the zoomed image and the pointer smoother. Windows smooths the image as it scales it, which can make the view and pointer shake slightly while you zoom. Wind compensates as well as it can, but this limits some features, such as the zoom glide after you let go, which stops a little early."
            checkLabel="Don't show this again" bind:checked={highResSkip}
            onEsc={cancelHighRes}
            buttons={[{ label: 'Cancel', onClick: cancelHighRes },
                      { label: 'Turn on', kind: 'primary', onClick: acceptHighRes }]} />
  {/if}
  {#if closePrompt}
    <Prompt id="close" title="Unsaved changes"
            text="You have changes that are not saved. Save them to the profile, discard them, or keep them for this session (they apply until Wind quits)."
            onEsc={() => (closePrompt = false)}
            buttons={[{ label: 'Keep for this session', onClick: () => closeWith('keep') },
                      { label: 'Discard', onClick: () => closeWith('discard') },
                      { label: 'Save', kind: 'primary', onClick: () => closeWith('save') }]} />
  {/if}
  {#if quitPrompt}
    <Prompt id="quit" title="Quit Wind?"
            text="You have changes that are not saved. Save them before Wind quits, or discard them."
            onEsc={() => (quitPrompt = false)}
            buttons={[{ label: 'Cancel', onClick: () => (quitPrompt = false) },
                      { label: 'Discard', onClick: () => quitWith('discard') },
                      { label: 'Save', kind: 'primary', onClick: () => quitWith('save') }]} />
  {/if}
  {#if profilePrompt}
    <Prompt id="profile" title="Unsaved changes"
            text="You have changes that are not saved. Save or discard them before changing profile."
            onEsc={() => (profilePrompt = null)}
            buttons={[{ label: 'Cancel', onClick: () => (profilePrompt = null) },
                      { label: 'Discard', onClick: () => resolveProfilePrompt('discard') },
                      { label: 'Save', kind: 'primary', onClick: () => resolveProfilePrompt('save') }]} />
  {/if}
  {#if newDialog}
    <NewProfileDialog names={prof.names} onCancel={() => (newDialog = false)}
                      onCreate={({ name, from }) => { newDialog = false; profileAction('create', { name, from }); }} />
  {/if}
  {#if deleteTarget}
    <Prompt id="pdel" title="Delete profile" text={'Delete "' + deleteTarget + '"? This cannot be undone.'}
            onEsc={() => (deleteTarget = '')}
            buttons={[{ label: 'Cancel', onClick: () => (deleteTarget = '') },
                      { label: 'Delete', kind: 'danger', onClick: () => { const name = deleteTarget; deleteTarget = ''; profileAction('delete', { name }); } }]} />
  {/if}
  {#if restartError}
    <Prompt id="rst" title="Couldn't restart Wind"
            text="Wind.exe could not be launched. The magnifier is still running with the previous engine."
            onEsc={() => (restartError = false)}
            buttons={[{ label: 'Close', kind: 'primary', onClick: () => (restartError = false) }]} />
  {/if}
  {#if dropped.length}
    <Prompt id="drop" title="Some keybinds were removed"
            text={'Wind no longer allows binds that would stop you typing a key or clash with Windows, so these are now unbound: ' + dropped.map((d) => d.label).join(', ') + '. Set them again with a modifier such as Ctrl, Alt or Win.'}
            onEsc={() => (dropped = [])}
            buttons={[{ label: 'OK', kind: 'primary', onClick: () => (dropped = []) }]} />
  {/if}
  {#if writeError}
    <Prompt id="wr" title="Couldn't save the setting"
            text={'Wind could not write "' + writeError + '" to its settings file, so the change did not stick. Another program may be holding the file. Try again in a moment.'}
            onEsc={() => (writeError = '')}
            buttons={[{ label: 'Close', kind: 'primary', onClick: () => (writeError = '') }]} />
  {/if}
  {#if saveError}
    <Prompt id="sv" title="Couldn't save"
            text="Wind could not write the profile file, so your changes are still unsaved. Another program may be holding the file. Try again in a moment."
            onEsc={() => (saveError = false)}
            buttons={[{ label: 'Close', kind: 'primary', onClick: () => (saveError = false) }]} />
  {/if}
  {#if profileError}
    <Prompt id="pe" title="Profile action failed" text={profileError}
            onEsc={() => (profileError = '')}
            buttons={[{ label: 'Close', kind: 'primary', onClick: () => (profileError = '') }]} />
  {/if}
</div>

<style>
  /* Hidden until the session is loaded, so no empty controls flash; the host paints the theme background behind it. */
  .app.pending { visibility: hidden; }
  .app { --side-w: 240px; width: 100vw; height: 100vh; display: grid; grid-template-rows: 38px 1fr; position: relative; overflow: hidden; }
  .body { display: grid; grid-template-columns: var(--side-w) minmax(0, 1fr); min-height: 0; }
  /* Narrow window: the sidebar moves above the page as a wrapped strip instead of eating the content. */
  @media (max-width: 700px) {
    .app { --side-w: 0px; }
    .body { grid-template-columns: minmax(0, 1fr); grid-template-rows: auto minmax(0, 1fr); }
  }
  .body { position: relative; }   /* the scroll chip sits on the right edge of the page column */
  /* No native scrollbar on the page: ScrollChip shows the position instead (#329). */
  /* The page is a size container: --gx is the side gutter (smaller when narrow) and caps the content at 960px on wide windows. */
  .main { position: relative; min-height: 0; min-width: 0; overflow-y: auto; overflow-x: hidden; padding: 0; outline: none; scrollbar-width: none;
          container-type: inline-size; }
  .page, .tail { --gx: max(clamp(16px, 4cqw, 40px), calc((100cqw - 960px) / 2)); padding-inline: var(--gx); }
  .main[data-page="tray"] :global(.banner + .card) { margin-top: 20px; }   /* k01: a caption-less first card sits 20px under the band */
  .page.fade { animation: pagein var(--dur-fast) var(--ease); }
  @keyframes pagein { from { opacity: 0; } }
  .tail { height: 110px; padding: 0; }   /* clearance so the capsule never covers the last row */
  .main[data-page="tray"] .tail { height: 120px; }   /* k01: the scroller pads 120px under the last card */
  .sr-only { position: absolute; width: 1px; height: 1px; margin: -1px; padding: 0; overflow: hidden;
             clip: rect(0 0 0 0); white-space: nowrap; border: 0; }
</style>
