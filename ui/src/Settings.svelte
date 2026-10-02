<script>
  // Settings window: instant apply with an explicit Save (spec 2026-10-01).
  //   values  the session: what the live ini holds right now. Every change is written at once
  //           (setConfig), so it takes effect immediately.
  //   saved   what the active profile file holds. The capsule counts the keys where the two differ.
  // Keybind captures are the exception: they write the live ini AND the profile (setConfigPersist),
  // so they are never "unsaved" and a later Save cannot lose them.
  import { onMount, tick } from 'svelte';
  import './design/tokens.css';
  import { groups } from './settings-schema.js';
  import { getSession, setConfig, setConfigPersist, saveSession, discardSession, openIni,
           exportDiagnostics, openRepo, pickExe, windowControl, onMessage, getMpoState,
           setMpoDisabled, rebootNow, setDirty, switchProfile, createProfile, renameProfile,
           duplicateProfile, deleteProfile } from './bridge.js';
  import { fill, changedKeys } from './session.js';
  import { applyTheme, setTheme } from './theme.js';
  import { droppedBinds } from './lib/keybindRules.js';
  import TitleBar from './shell/TitleBar.svelte';
  import Sidebar from './shell/Sidebar.svelte';
  import Banner from './shell/Banner.svelte';
  import Card from './shell/Card.svelte';
  import SaveCapsule from './shell/SaveCapsule.svelte';
  import SettingRow from './controls/SettingRow.svelte';
  import Prompt from './prompts/Prompt.svelte';
  import Results from './search/Results.svelte';
  import { search } from './search/search.js';

  // Replaced at build time with src/version.h's WIND_VERSION_STR (vite.config.js); empty in tests.
  const VERSION = typeof __WIND_VERSION__ === 'string' ? __WIND_VERSION__ : '';

  let values = $state({});
  let saved = $state({});
  let themeMode = $state('auto');
  let prof = $state({ names: [], active: '' });
  let activeId = $state('zoom');
  let loaded = $state(false);
  let main = $state();

  const side = groups.filter((g) => g.id !== 'advanced' && g.id !== 'about');
  const expert = groups.filter((g) => g.id === 'advanced' || g.id === 'about');
  const group = $derived(groups.find((g) => g.id === activeId) || groups[0]);
  const count = $derived(loaded ? changedKeys(values, saved).length : 0);
  const dirty = $derived(count > 0);
  $effect(() => { if (loaded) setDirty(dirty); });   // the host's WM_CLOSE guard follows the UI

  // Effective palette: auto resolves against the system setting.
  let systemDark = $state(typeof matchMedia === 'function' && matchMedia('(prefers-color-scheme: dark)').matches);
  const effTheme = $derived(themeMode === 'light' ? 'light' : themeMode === 'dark' ? 'dark' : (systemDark ? 'dark' : 'light'));

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
    themeMode = s.theme || 'auto'; applyTheme(themeMode);
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
      onMessage((m) => { if (m && m.type === 'windowState') document.documentElement.toggleAttribute('data-maximized', !!m.maximized); }),
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
    const mq = typeof matchMedia === 'function' ? matchMedia('(prefers-color-scheme: dark)') : null;
    const onMq = (e) => { systemDark = e.matches; };
    if (mq && mq.addEventListener) mq.addEventListener('change', onMq);
    (async () => {
      await load();
      const s = await getMpoState();
      mpoLive = s.disabled; mpoKnown = true;
      // No record for this boot -> assume the registry is what DWM loaded.
      mpoBoot = s.bootKnown ? s.atBoot : s.disabled;
    })();
    return () => { offs.forEach((o) => o()); if (mq && mq.removeEventListener) mq.removeEventListener('change', onMq); };
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
  // Switching, creating and deleting the ACTIVE profile replace the session, so with unsaved changes
  // they ask first (Save / Discard / Cancel). Rename, duplicate and deleting another profile do not.
  let profilePrompt = $state(null);   // pending { kind, payload }
  let profileError = $state('');
  function profileAction(kind, payload) {
    const deletesActive = kind === 'delete' && payload.name.toLowerCase() === prof.active.toLowerCase();
    const replaces = kind === 'switch' || kind === 'create' || deletesActive;
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
    else if (kind === 'create') r = await createProfile(payload.name);
    else if (kind === 'rename') r = await renameProfile(payload.from, payload.to);
    else if (kind === 'duplicate') r = await duplicateProfile(payload.name);
    else if (kind === 'delete') r = await deleteProfile(payload.name);
    else return;
    prof = { names: r.names, active: r.active };
    // Reload BEFORE the error check: a failed op can still have rewritten the live ini.
    if (kind === 'switch' || kind === 'create' || (kind === 'delete' && r.active !== prevActive)) await load();
    if (!r.ok) { profileError = r.error || 'Profile operation failed'; return; }
    if (kind === 'switch') announce('Switched to profile ' + payload.name + '. Settings reloaded.');
    else if (kind === 'create') { announce('Created profile ' + payload.name + '. Settings reset to defaults.'); select('zoom'); }
    else if (kind === 'rename') announce('Renamed profile to ' + payload.to);
    else if (kind === 'duplicate') announce('Duplicated profile ' + payload.name);
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
  function onTheme(mode) { themeMode = mode; setTheme(mode); announce('Theme ' + mode); }
  function onAction(a) {
    if (a === 'openIni') openIni();
    else if (a === 'exportDiagnostics') exportDiagnostics();
    else if (a === 'quitWind') requestQuit();
  }
  const visible = (r) => !r.showIf || String(values[r.showIf.key]) === String(r.showIf.eq);
  const extra = $derived({
    mpoNeedsRestart, runningModel, version: VERSION, theme: themeMode, onTheme, onRepo: openRepo,
    onAction, pick: pickExe,
    onRestart: () => { restartError = false; windowControl('restartWind'); },
    profiles: {
      names: prof.names, active: prof.active,
      onSwitch: (name) => profileAction('switch', { name }),
      onCreate: (name) => profileAction('create', { name }),
      onRename: (from, to) => profileAction('rename', { from, to }),
      onDuplicate: (name) => profileAction('duplicate', { name }),
      onDelete: (name) => profileAction('delete', { name }),
    },
  });
</script>

<svelte:window onkeydown={onKeydown} />

<div class="wnd app" data-theme={effTheme}>
  <TitleBar {themeMode} onTheme={onTheme} onMinimize={() => windowControl('minimize')} onClose={requestClose} />
  <div class="body">
    <Sidebar groups={side} {expert} active={searching ? '' : activeId} version={VERSION} {query} {onSearch}
             onSelect={(id) => { clearSearch(); select(id); }} />
    <main class="main" bind:this={main} tabindex="-1" aria-label={group.label}>
      {#key searching ? '?search' : activeId}
      <div class="page" class:fade={navigated}>
      {#if searching}
        <Results {results} {query} onJump={jump} />
      {:else}
      <Banner title={group.label} description={group.desc} icon={group.icon} />
      {#each group.cards as card, i (group.id + i)}
        <Card caption={card.caption}>
          {#each card.rows.filter(visible) as r (r.key)}
            <SettingRow row={r} value={values[r.key]} {values} {extra} {live}
                        onChange={(v) => change(r.key, v)} />
          {/each}
        </Card>
      {/each}
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
  .app { width: 100vw; height: 100vh; display: grid; grid-template-rows: 38px 1fr; position: relative; overflow: hidden; }
  .body { display: grid; grid-template-columns: 240px 1fr; min-height: 0; }
  .main { position: relative; min-height: 0; overflow-y: auto; padding: 0 40px; outline: none;
          scrollbar-width: thin; scrollbar-color: color-mix(in srgb, var(--fg) 16%, transparent) transparent; }
  .page.fade { animation: pagein var(--dur-fast) var(--ease); }
  @keyframes pagein { from { opacity: 0; } }
  .tail { height: 110px; }   /* clearance so the capsule never covers the last row */
  .sr-only { position: absolute; width: 1px; height: 1px; margin: -1px; padding: 0; overflow: hidden;
             clip: rect(0 0 0 0); white-space: nowrap; border: 0; }
</style>
