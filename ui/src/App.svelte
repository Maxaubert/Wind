<script>
  import { onMount } from 'svelte';
  import { sections } from './settings-schema.js';
  import { getConfig, setConfig } from './bridge.js';
  import Row from './lib/Row.svelte';
  let active = sections[0].id;
  let values = {};
  let saved = {};
  onMount(async () => {
    const cfg = await getConfig();
    const v = {};
    for (const s of sections) for (const r of s.rows) v[r.key] = (r.key in cfg) ? cfg[r.key] : r.def;
    values = v; saved = { ...v };
  });
  function change(key, val) { values = { ...values, [key]: val }; }   // stage only; Apply writes
  function apply() {
    for (const k of Object.keys(values))
      if (String(values[k]) !== String(saved[k])) setConfig(k, values[k]);
    saved = { ...values };
  }
  function discard() { values = { ...saved }; }
  $: section = sections.find(s => s.id === active);
  $: dirty = Object.keys(values).some(k => String(values[k]) !== String(saved[k]));
</script>
<div class="app">
  <nav>{#each sections as s}<button class:on={s.id === active} on:click={() => active = s.id}>{s.label}</button>{/each}</nav>
  <div class="content">
    <main>
      <h1>{section.label}</h1>
      {#each section.rows as r}<Row row={r} value={values[r.key]} onChange={v => change(r.key, v)} />{/each}
    </main>
    <footer>
      <span class="hint">{dirty ? 'Unsaved changes' : 'All changes saved'}</span>
      <button class="discard" on:click={discard} disabled={!dirty}>Discard</button>
      <button class="apply" on:click={apply} disabled={!dirty}>Apply</button>
    </footer>
  </div>
</div>
<style>
  :root{--bg:#fff;--fg:#111;--muted:#666;--border:#e5e5e5;--accent:#5b5bd6;--side:#f5f5f7}
  @media (prefers-color-scheme: dark){:root{--bg:#1a1a1a;--fg:#eee;--muted:#999;--border:#333;--side:#222}}
  :global(html,body){margin:0;height:100%;background:var(--bg);color:var(--fg);font-family:system-ui,sans-serif}
  /* Hardcode control accent to the tab-blue (not the OS theme accent) */
  :global(input[type=checkbox]),:global(input[type=range]){accent-color:var(--accent)}
  .app{display:flex;height:100vh}
  nav{width:200px;background:var(--side);padding:16px 8px;display:flex;flex-direction:column;gap:4px}
  nav button{text-align:left;padding:10px 12px;border:0;border-radius:8px;background:transparent;color:var(--fg);cursor:pointer;font-size:1em}
  nav button.on{background:var(--accent);color:#fff}
  .content{flex:1;min-width:0;display:flex;flex-direction:column}
  main{flex:1;overflow:auto;padding:24px 32px}
  footer{display:flex;align-items:center;gap:12px;padding:12px 32px;border-top:1px solid var(--border);background:var(--bg)}
  footer .hint{margin-right:auto;color:var(--muted);font-size:.9em}
  footer button{padding:8px 18px;border-radius:8px;border:1px solid var(--border);background:transparent;color:var(--fg);cursor:pointer;font-size:.95em}
  footer button.apply{background:var(--accent);color:#fff;border-color:var(--accent)}
  footer button:disabled{opacity:.5;cursor:default}
</style>
