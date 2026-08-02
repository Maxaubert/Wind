<script>
  import { onMount } from 'svelte';
  import { sections } from './settings-schema.js';
  import { getConfig, setConfig, openIni, exportDiagnostics, windowControl, onMessage,
           getMpoState, setMpoDisabled, rebootNow, setDirty } from './bridge.js';
  import { currentTheme, applyTheme, nextTheme, setTheme } from './theme.js';
  import Rail from './lib/Rail.svelte';
  import Section from './lib/Section.svelte';
  import Row from './lib/Row.svelte';
  import { ic } from './lib/icons.js';
  import { scrollspy, scrollToSection } from './lib/scrollspy.js';

  let values = {}, saved = {}, active = sections[0].id, theme = 'auto', scroller;
  const railItems = sections.map(s => ({ id: s.id, label: s.label, icon: s.icon }));
  const ids = sections.map(s => s.id);

  onMount(async () => {
    const cfg = await getConfig();
    const v = {};
    for (const s of sections) for (const r of s.rows) if (r.key[0] !== '_') v[r.key] = (r.key in cfg) ? cfg[r.key] : r.def;
    // These must match the core's shipped defaults (src/config.h + the ini template in config.cpp),
    // which are ALL unbound - onboarding captures the user's choice. Seeding a key here that the
    // core does not default to (this used to be PageUp 33 / PageDown 34) invents a binding the user
    // never chose and can write it into the ini on the next Apply.
    const kbDefaults = { zoomInButton:'0', zoomInVk:'0', zoomOutButton:'0', zoomOutVk:'0',
                         zoomInButton2:'0', zoomOutButton2:'0',
                         zoomInVk2:'0', zoomOutVk2:'0',
                         zoomInMods:'0', zoomOutMods:'0', zoomInMods2:'0', zoomOutMods2:'0',
                         hideCursorVk:'0', hideCursorMods:'0',
                         swapModelVk:'0',
                         quickZoomVk:'112', quickZoomMods:'0' };
    for (const k of Object.keys(kbDefaults)) v[k] = (k in cfg) ? cfg[k] : kbDefaults[k];
    values = v; saved = { ...v };
    theme = currentTheme(cfg); applyTheme(theme);
    // MPO lives in the registry, not the ini, so it is fetched separately and staged separately.
    const disabled = await getMpoState();
    mpoLive = disabled; mpoStaged = disabled; mpoKnown = true;
  });
  // --- MPO (issue #164) -----------------------------------------------------
  // mpoLive  = what the registry says right now. mpoStaged = what the toggle shows.
  // They differ only while a change is staged; Apply reconciles them through an elevated write.
  let mpoLive = false, mpoStaged = false, mpoKnown = false;
  let mpoRestartPrompt = false, mpoFailed = false;
  $: mpoDirty = mpoKnown && mpoStaged !== mpoLive;
  $: extra = { mpoKnown, mpoLive, mpoStaged };
  // Live setter for keybind rows: writes setConfig immediately AND updates both staged + saved
  // so the rebind is effective at once (the core hot-reloads it, the hook stops swallowing the
  // previous binding) and the Apply/Discard footer does NOT show keybind changes as dirty.
  function live(patch) {
    for (const k of Object.keys(patch)) setConfig(k, patch[k]);
    values = { ...values, ...patch };
    saved = { ...saved, ...patch };
  }
  // A model change is applied by writing the ini then relaunching Wind (model is read once at
  // launch, so a hot-reload can't switch it). To the user a deliberate Apply and a hot-reload look
  // identical, so there is no confirm step. restartError still surfaces a relaunch that failed.
  let restartError = false;
  // The model that the LIVE process is running. Captured before commit() overwrites saved.model, so
  // a failed relaunch can revert the ini + dropdown back to it (keeps ini model == running model).
  let runningModel = '';
  onMessage(m => {
    if (m && m.type === 'restartFailed') {
      values = { ...values, model: runningModel };
      saved  = { ...saved,  model: runningModel };
      setConfig('model', runningModel);   // rewrite the ini back to the model still running
      restartError = true;
    }
  });
  function change(keyOrPatch, val) {
    // Atomic multi-key form: change({k1:v1, k2:v2}) updates both in a single render. The keybind
    // capture uses this so its sibling-key update (vk + button) lands as one consistent state.
    if (keyOrPatch && typeof keyOrPatch === 'object') {
      values = { ...values, ...keyOrPatch };
      return;
    }
    const key = keyOrPatch;
    if (key === '__action') { if (val === 'openIni') openIni(); return; }
    // Staged only - the elevated write happens in apply(), like every other setting.
    if (key === '__mpoStaged') { mpoStaged = !!val; return; }
    const next = { ...values, [key]: val };
    // Disabling the alternate-keybinds toggle clears the alternate bindings (side-button + key) so
    // any previously bound alt input stops firing (Apply still has to be pressed to persist).
    if (key === 'altKeybinds' && Number(val) === 0) {
      next.zoomInVk2 = '0'; next.zoomOutVk2 = '0';
      next.zoomInButton2 = '0'; next.zoomOutButton2 = '0';
    }
    values = next;
  }
  function commit() {
    for (const k of Object.keys(values)) if (String(values[k]) !== String(saved[k])) setConfig(k, values[k]);
    saved = { ...values };
  }
  // `model` is read once at Wind launch, so switching it writes the ini (model FIRST - the relaunched
  // Wind reads it at startup) then relaunches. No confirm: a restart and a hot-reload look the same.
  async function apply() {
    // MPO first and awaited: it raises a UAC prompt, and a cancelled prompt must revert the toggle
    // rather than leave the row claiming a change that never reached the registry.
    if (mpoDirty) {
      const want = mpoStaged;
      const res = await setMpoDisabled(want);
      mpoLive = res.disabled; mpoStaged = res.disabled;
      if (res.ok && res.disabled === want) mpoRestartPrompt = true;   // reboot for DWM to read it
      else mpoFailed = true;
    }
    if (String(values.model) !== String(saved.model)) {
      runningModel = saved.model;   // remember what's live before commit() moves saved.model forward
      restartError = false;
      commit();
      windowControl('restartWind');
      return;
    }
    commit();
  }
  function discard() { values = { ...saved }; mpoStaged = mpoLive; }
  // --- Unsaved-changes guard (issue #164) -----------------------------------
  // The host mirrors `dirty` and bounces WM_CLOSE back as confirmClose, so Alt+F4 and the system
  // menu get the same prompt as our own title-bar button. Closing to the tray still loses staged
  // edits, which is exactly why it is worth asking.
  let closePrompt = false;
  function requestClose() { if (dirty) closePrompt = true; else windowControl('close'); }
  function discardAndClose() { closePrompt = false; windowControl('close', true); }
  onMessage(m => { if (m && m.type === 'confirmClose') closePrompt = true; });
  function toggleTheme() { theme = nextTheme(theme); setTheme(theme); }
  $: dirty = Object.keys(values).some(k => String(values[k]) !== String(saved[k])) || mpoDirty;
  $: setDirty(dirty);   // keep the host's WM_CLOSE guard in step with the staged state
  // Advanced rows (schema `advanced:true`) are hidden unless "Show advanced settings" is on. Driven
  // by the live `values` so toggling it reveals/hides rows immediately (before Apply).
  $: advancedOn = Number(values.showAdvanced) === 1;
</script>
<div class="win">
  <Rail sections={railItems} {active} onSelect={(id) => scrollToSection(scroller, id)}
        {theme} onToggleTheme={toggleTheme} />
  <section class="content">
    <div class="caption" style="app-region:drag;-webkit-app-region:drag">
      <span class="ctitle">Wind Settings</span>
      <div class="tbtns" style="app-region:no-drag;-webkit-app-region:no-drag">
        <button class="tbtn" title="Minimize" aria-label="Minimize" on:click={() => windowControl('minimize')}>{@html ic.min}</button>
        <button class="tbtn close" title="Close" aria-label="Close" on:click={requestClose}>{@html ic.close}</button>
      </div>
    </div>
    <div class="scroll" bind:this={scroller}
         use:scrollspy={{ sectionIds: ids, onActive: (id) => active = id }}>
      {#each sections as s}
        <Section id={s.id} label={s.label} desc={s.desc}>
          {#each s.rows as r}
            {#if (!r.requires || Number(values[r.requires]) === 1) && (!r.requiresNot || Number(values[r.requiresNot]) !== 1) && (!r.advanced || advancedOn) && (!r.showIf || values[r.showIf.key] === r.showIf.eq)}
              <Row row={r} value={values[r.key]} {values} {extra} set={change} {live}
                   disabled={r.dependsOn && Number(values[r.dependsOn]) !== 1}
                   onChange={(val) => change(r.key, val)} />
            {/if}
          {/each}
        </Section>
      {/each}
    </div>
    <footer>
      <button class="btn" on:click={exportDiagnostics}>Export diagnostics</button>
      <button class="btn" on:click={discard} disabled={!dirty}>Discard</button>
      <button class="btn primary" on:click={apply} disabled={!dirty}>Apply</button>
    </footer>
  </section>
  {#if restartError}
    <div class="mbackdrop">
      <div class="mbox" role="dialog" aria-modal="true" aria-labelledby="rtitle">
        <h2 id="rtitle">Couldn't restart Wind</h2>
        <p>Wind.exe could not be launched. The magnifier is still running with the previous model.</p>
        <div class="mbtns"><button class="primary" on:click={() => (restartError = false)}>Close</button></div>
      </div>
    </div>
  {/if}
  {#if mpoRestartPrompt}
    <div class="mbackdrop">
      <div class="mbox" role="dialog" aria-modal="true" aria-labelledby="mtitle">
        <h2 id="mtitle">Restart to finish</h2>
        <p>
          MPO is now {mpoLive ? 'disabled' : 'enabled'} in the registry. Windows only reads this
          setting when it starts, so it takes effect after a restart.
        </p>
        <div class="mbtns">
          <button on:click={() => (mpoRestartPrompt = false)}>Cancel</button>
          <button class="primary" on:click={() => { mpoRestartPrompt = false; rebootNow(); }}>Restart now</button>
        </div>
      </div>
    </div>
  {/if}
  {#if mpoFailed}
    <div class="mbackdrop">
      <div class="mbox" role="dialog" aria-modal="true" aria-labelledby="ftitle">
        <h2 id="ftitle">MPO change not applied</h2>
        <p>
          The registry was not changed. This happens if the administrator prompt was dismissed.
          Nothing else in your settings was affected.
        </p>
        <div class="mbtns"><button class="primary" on:click={() => (mpoFailed = false)}>Close</button></div>
      </div>
    </div>
  {/if}
  {#if closePrompt}
    <div class="mbackdrop">
      <div class="mbox" role="dialog" aria-modal="true" aria-labelledby="ctitle">
        <h2 id="ctitle">Settings not applied</h2>
        <p>You have changes that haven't been applied. Closing now discards them.</p>
        <div class="mbtns">
          <button on:click={() => (closePrompt = false)}>Cancel</button>
          <button class="primary" on:click={discardAndClose}>Discard</button>
        </div>
      </div>
    </div>
  {/if}
</div>
<style>
  /* Ported from mockups/config-ui-onepage.html (.win / .main->.content / .caption / .ctitle /
     .tbtns / .tbtn / .scroll / .footer / .fhint->.hint / .btn). */
  .win { width: 100vw; height: 100vh; overflow: hidden; display: flex;
         background: var(--bg); color: var(--text); font-size: 13px; }
  .content { flex: 1; min-width: 0; display: flex; flex-direction: column; }
  .caption { height: 44px; flex-shrink: 0; display: flex; align-items: center; justify-content: space-between; padding-left: 22px; }
  .ctitle { font-size: 12.5px; color: var(--muted); font-weight: 500; }
  .tbtns { display: flex; height: 100%; }
  .tbtn { width: 46px; height: 100%; display: grid; place-items: center; color: var(--muted); border: 0; background: transparent; cursor: pointer; }
  .tbtn:hover { background: var(--hover); color: var(--text); }
  .tbtn.close:hover { background: #e81123; color: #fff; }
  .scroll { flex: 1; overflow-y: auto; position: relative; padding: 0 30px 24px;
            scrollbar-width: thin; scrollbar-color: var(--track) transparent; }
  /* Slim themed scrollbar (Chromium/WebView2). The 'thin' rule above handles Firefox. */
  .scroll::-webkit-scrollbar { width: 8px; }
  .scroll::-webkit-scrollbar-track { background: transparent; }
  .scroll::-webkit-scrollbar-thumb { background: var(--track); border-radius: 4px; border: 2px solid transparent; background-clip: padding-box; }
  .scroll::-webkit-scrollbar-thumb:hover { background: var(--accent-icon); background-clip: padding-box; border: 2px solid transparent; }
  /* Extra breathing room between sections (Section.svelte's .sec lives in another scope,
     so use :global to reach it). The About section's large logo hero gives the last section
     real height so its header can reach the scroll-spy top band; the scrollspy also has a
     bottom-of-scroll fallback that activates the last section when the container hits bottom. */
  :global(.sec + .sec) { margin-top: 40px; }
  footer { flex-shrink: 0; display: flex; align-items: center; justify-content: flex-end; gap: 10px; padding: 12px 26px; border-top: 1px solid var(--line); }
  .hint { margin-right: auto; color: var(--muted); font-size: 11.5px; }
  .btn { padding: 7px 16px; border-radius: 7px; border: 1px solid var(--line); background: transparent; color: var(--text); font-size: 12.5px; cursor: pointer; }
  .btn.primary { background: var(--accent); border-color: var(--accent); color: #fff; }
  .btn:disabled { opacity: .5; cursor: default; }
  .mbackdrop { position: fixed; inset: 0; background: rgba(0,0,0,.45);
               display: flex; align-items: center; justify-content: center; z-index: 50; }
  .mbox { background: var(--bg); color: var(--text); border: 1px solid var(--line); border-radius: 10px;
          padding: 20px 22px; width: 380px; box-shadow: 0 12px 40px rgba(0,0,0,.5); }
  .mbox h2 { margin: 0 0 8px; font-size: 15px; }
  .mbox p { margin: 0 0 18px; font-size: 13px; opacity: .85; line-height: 1.45; }
  .mbtns { display: flex; gap: 8px; justify-content: flex-end; }
  .mbtns button { padding: 7px 16px; border-radius: 7px; font-size: 12.5px; cursor: pointer;
                  border: 1px solid var(--line); background: transparent; color: var(--text); }
  .mbtns button.primary { background: var(--accent); border-color: var(--accent); color: #fff; }
</style>
