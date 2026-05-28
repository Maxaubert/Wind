<script>
  import { onMount } from 'svelte';
  import { sections } from './settings-schema.js';
  import { getConfig, setConfig, openIni, windowControl } from './bridge.js';
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
    const kbDefaults = { zoomInButton:'2', zoomInVk:'33', zoomOutButton:'1', zoomOutVk:'34',
                         zoomInVk2:'0', zoomOutVk2:'0' };
    for (const k of Object.keys(kbDefaults)) v[k] = (k in cfg) ? cfg[k] : kbDefaults[k];
    values = v; saved = { ...v };
    theme = currentTheme(cfg); applyTheme(theme);
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
    const next = { ...values, [key]: val };
    // Disabling the alternate-keybinds toggle clears the alternate VK fields so any previously
    // bound alt keys stop firing (Apply still has to be pressed to persist).
    if (key === 'altKeybinds' && Number(val) === 0) { next.zoomInVk2 = '0'; next.zoomOutVk2 = '0'; }
    values = next;
  }
  function apply() {
    for (const k of Object.keys(values)) if (String(values[k]) !== String(saved[k])) setConfig(k, values[k]);
    saved = { ...values };
  }
  function discard() { values = { ...saved }; }
  function toggleTheme() { theme = nextTheme(theme); setTheme(theme); }
  $: dirty = Object.keys(values).some(k => String(values[k]) !== String(saved[k]));
</script>
<div class="win">
  <Rail sections={railItems} {active} onSelect={(id) => scrollToSection(scroller, id)}
        {theme} onToggleTheme={toggleTheme} />
  <section class="content">
    <div class="caption" style="app-region:drag;-webkit-app-region:drag">
      <span class="ctitle">Wind Settings</span>
      <div class="tbtns" style="app-region:no-drag;-webkit-app-region:no-drag">
        <button class="tbtn" title="Minimize" aria-label="Minimize" on:click={() => windowControl('minimize')}>{@html ic.min}</button>
        <button class="tbtn close" title="Close" aria-label="Close" on:click={() => windowControl('close')}>{@html ic.close}</button>
      </div>
    </div>
    <div class="scroll" bind:this={scroller}
         use:scrollspy={{ sectionIds: ids, onActive: (id) => active = id }}>
      {#each sections as s}
        <Section id={s.id} label={s.label} desc={s.desc}>
          {#each s.rows as r}
            {#if !r.requires || Number(values[r.requires]) === 1}
              <Row row={r} value={values[r.key]} {values} set={change}
                   disabled={r.dependsOn && Number(values[r.dependsOn]) !== 1}
                   onChange={(val) => change(r.key, val)} />
            {/if}
          {/each}
        </Section>
      {/each}
    </div>
    <footer>
      <button class="btn" on:click={discard} disabled={!dirty}>Discard</button>
      <button class="btn primary" on:click={apply} disabled={!dirty}>Apply</button>
    </footer>
  </section>
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
</style>
