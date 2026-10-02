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
           exportDiagnostics, openRepo, pickExe, windowControl, onMessage, getMpoState,
           setMpoDisabled, rebootNow, setDirty, switchProfile, createProfile, deleteProfile } from './bridge.js';
  import { fill, changedKeys, GLOBAL_KEYS } from './session.js';
  import { themes, normalizePalette } from './design/themes.js';
  import { droppedBinds } from './lib/keybindRules.js';
  import TitleBar from './shell/TitleBar.svelte';
  import Sidebar from './shell/Sidebar.svelte';
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
  let revealKey = $state('');   // an advanced row opened by a search result while the switch is off

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
    if (!runningModel) runningModel = String(f.values.model);
    loaded = true;
  }

  // --- MPO (issue #164, #242) -----------------------------------------------------------------
  // High resolution cursor couples to the registry. The registry write needs UAC, so it runs the
  // moment the toggle moves; a dismissed prompt reverts the toggle. mpoBoot is what DWM loaded at
  // boot and alone decides whether a restart is required.
  let mpoLive = $state(false), mpoBoot = $state(false), mpoKnown = $state(false);
  let mpoRestartPrompt = $state(false), mpoFailed = $state(false);
  const mpoNeedsRestart = $derived(mpoKnown && mpoLive !== mpoBoot);
  async function syncMpo(prevTx) {
    if (!mpoKnown) return;
    const want = Number(values.txSamplingMode) !== 1;   // crisp sampling -> MPO disabled
    if (want === mpoLive) return;
    const res = await setMpoDisabled(want);
    mpoLive = res.disabled;
    if (!res.ok || res.disabled !== want) {
      mpoFailed = true;
      // The ini half must not land alone: crisp sampling with MPO still on is the driver-crash combo.
      values = { ...values, txSamplingMode: prevTx }; setConfig('txSamplingMode', prevTx);
    } else if (res.disabled !== mpoBoot) mpoRestartPrompt = true;
  }

  // --- Changes --------------------------------------------------------------------------------
  function change(key, val) {
    if (key === 'model') announce('Magnifier model set to ' + val + '. Some display options changed.');
    if (key === 'uiPalette') announce('Theme ' + (themes.find((t) => t.id === val)?.label ?? val));
    const prev = values[key];
    values = { ...values, [key]: val };
    setConfig(key, val);
    if (key === 'txSamplingMode') syncMpo(prev);
  }
  // Keybind captures: live AND saved at once, so the hook stops swallowing the old binding and a
  // later Save or Discard cannot lose them.
  function live(patch) {
    for (const k of Object.keys(patch)) setConfigPersist(k, patch[k]);
    values = { ...values, ...patch };
    saved = { ...saved, ...patch };
  }
  let restartError = $state(false);
  let writeError = $state('');
  let saveError = $state(false);
  onMount(() => {
    const offs = [
      onMessage((m) => { if (m && m.type === 'configWriteFailed') writeError = m.key || 'a setting'; }),
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
      const s = await getMpoState();
      mpoLive = s.disabled; mpoKnown = true;
      // No record for this boot -> assume the registry is what DWM loaded.
      mpoBoot = s.bootKnown ? s.atBoot : s.disabled;
    })();
    return () => { offs.forEach((o) => o()); };
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
    const snapshot = { ...values };   // what "start from the current settings" copies
    let r;
    if (kind === 'switch') r = await switchProfile(payload.name);
    else if (kind === 'create') r = await createProfile(payload.name);
    else if (kind === 'delete') r = await deleteProfile(payload.name);
    else return;
    prof = { names: r.names, active: r.active };
    // Reload BEFORE the error check: a failed op can still have rewritten the live ini.
    if (kind === 'switch' || kind === 'create' || (kind === 'delete' && r.active !== prevActive)) await load();
    if (!r.ok) { profileError = r.error || 'Profile operation failed'; return; }
    if (kind === 'switch') announce('Switched to profile ' + payload.name + '. Settings reloaded.');
    else if (kind === 'create' && payload.from === 'current') {
      // The host starts a new profile from the defaults, so write the current settings into it. Persisted
      // (profile file and live ini), so the new profile holds them and nothing shows as unsaved.
      const patch = {};
      for (const k of Object.keys(snapshot)) {
        if (GLOBAL_KEYS.has(k) || k[0] === '_') continue;
        if (String(snapshot[k]) !== String(values[k])) patch[k] = snapshot[k];
      }
      live(patch);
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
  async function jump(hit) {
    clearSearch();
    revealKey = hit.adv ? hit.key : '';
    await select(hit.groupId);
    const row = main && main.querySelector('[data-key="' + hit.key + '"]');
    if (row) {
      row.scrollIntoView({ block: 'center' });
      const f = row.querySelector('button, input, select, [tabindex]');
      if (f) f.focus({ preventScroll: true });
    }
  }
  function onKeydown(e) {
    const mod = e.ctrlKey && !e.altKey && !e.shiftKey;
    if (mod && e.key.toLowerCase() === 'f') { e.preventDefault(); const i = searchInput(); if (i) { i.focus(); i.select(); } return; }
    if (e.key === 'Escape' && (searching || e.target === searchInput())) {
      e.preventDefault(); clearSearch(); if (main) main.focus({ preventScroll: true }); return;
    }
    if (e.key === 'Enter' && e.target === searchInput() && results.length) { e.preventDefault(); jump(results[0].rows[0]); return; }
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
  const visible = (r) => !r.showIf || String(values[r.showIf.key]) === String(r.showIf.eq);
  // Advanced rows show only while the global switch is on, with one exception: the row a search result
  // opened (revealKey) shows on its own and turns nothing on.
  const advOn = $derived(Number(values.showAdvanced) === 1);
  const shown = (r) => visible(r) && (!r.adv || advOn || r.key === revealKey);
  const extra = $derived({
    mpoNeedsRestart, runningModel, version: VERSION, onRepo: openRepo,
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
             onSelect={(id) => { revealKey = ''; clearSearch(); select(id); }} />
    <main class="main" data-page={searching ? 'search' : group.id} bind:this={main} tabindex="-1" aria-label={group.label}>
      {#key searching ? '?search' : activeId}
      <div class="page" class:fade={navigated}>
      {#if searching}
        <Results {results} {query} onJump={jump} />
      {:else}
      <Banner title={group.label} description={group.desc} icon={group.icon} />
      {#each group.cards as card, i (group.id + i)}
        {@const rows = card.rows.filter(shown)}
        {#if rows.length}
        <Card caption={card.caption}>
          {#each rows as r (r.key)}
            <SettingRow row={r} value={values[r.key]} {values} {extra} {live}
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
  </div>
  <SaveCapsule {count} onSave={save} onDiscard={discard} />
  <div class="sr-only" role="status" aria-live="polite" aria-atomic="true">{announcement}</div>

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
    <NewProfileDialog names={prof.names} current={prof.active} onCancel={() => (newDialog = false)}
                      onCreate={({ name, from }) => { newDialog = false; profileAction('create', { name, from }); }} />
  {/if}
  {#if deleteTarget}
    <Prompt id="pdel" title="Delete profile" text={'Delete "' + deleteTarget + '"? This cannot be undone.'}
            onEsc={() => (deleteTarget = '')}
            buttons={[{ label: 'Cancel', onClick: () => (deleteTarget = '') },
                      { label: 'Delete', kind: 'danger', onClick: () => { const name = deleteTarget; deleteTarget = ''; profileAction('delete', { name }); } }]} />
  {/if}
  {#if mpoRestartPrompt}
    <Prompt id="mpo" title="Restart to finish"
            text={'MPO is now ' + (mpoLive ? 'disabled' : 'enabled') + ' in the registry. Windows only reads this setting when it starts, so it takes effect after a restart.'}
            onEsc={() => (mpoRestartPrompt = false)}
            buttons={[{ label: 'Cancel', onClick: () => (mpoRestartPrompt = false) },
                      { label: 'Restart now', kind: 'primary', onClick: () => { mpoRestartPrompt = false; rebootNow(); } }]} />
  {/if}
  {#if mpoFailed}
    <Prompt id="mpof" title="MPO change not applied"
            text="The registry was not changed. This happens if the administrator prompt was dismissed. Nothing else in your settings was affected."
            onEsc={() => (mpoFailed = false)}
            buttons={[{ label: 'Close', kind: 'primary', onClick: () => (mpoFailed = false) }]} />
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
  .app { width: 100vw; height: 100vh; display: grid; grid-template-rows: 38px 1fr; position: relative; overflow: hidden; }
  .body { display: grid; grid-template-columns: 240px 1fr; min-height: 0; }
  .main { position: relative; min-height: 0; overflow-y: auto; padding: 0 40px; outline: none; }
  .main[data-page="tray"] :global(.banner + .card) { margin-top: 20px; }   /* k01: a caption-less first card sits 20px under the band */
  .page.fade { animation: pagein var(--dur-fast) var(--ease); }
  @keyframes pagein { from { opacity: 0; } }
  .tail { height: 110px; }   /* clearance so the capsule never covers the last row */
  .main[data-page="tray"] .tail { height: 120px; }   /* k01: the scroller pads 120px under the last card */
  .sr-only { position: absolute; width: 1px; height: 1px; margin: -1px; padding: 0; overflow: hidden;
             clip: rect(0 0 0 0); white-space: nowrap; border: 0; }
</style>
