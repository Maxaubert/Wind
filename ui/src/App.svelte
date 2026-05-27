<script>
  import { onMount } from 'svelte';
  import { sections } from './settings-schema.js';
  import { getConfig, setConfig } from './bridge.js';
  import Row from './lib/Row.svelte';
  let active = sections[0].id;
  let values = {};
  onMount(async () => {
    const cfg = await getConfig();
    const v = {};
    for (const s of sections) for (const r of s.rows) v[r.key] = (r.key in cfg) ? cfg[r.key] : r.def;
    values = v;
  });
  function change(key, val) { values = { ...values, [key]: val }; setConfig(key, val); }
  $: section = sections.find(s => s.id === active);
</script>
<div class="app">
  <nav>{#each sections as s}<button class:on={s.id === active} on:click={() => active = s.id}>{s.label}</button>{/each}</nav>
  <main>
    <h1>{section.label}</h1>
    {#each section.rows as r}<Row row={r} value={values[r.key]} onChange={v => change(r.key, v)} />{/each}
  </main>
</div>
<style>
  :root{--bg:#fff;--fg:#111;--muted:#666;--border:#e5e5e5;--accent:#5b5bd6;--side:#f5f5f7}
  @media (prefers-color-scheme: dark){:root{--bg:#1a1a1a;--fg:#eee;--muted:#999;--border:#333;--side:#222}}
  :global(body){margin:0;background:var(--bg);color:var(--fg);font-family:system-ui,sans-serif}
  .app{display:flex;height:100vh}
  nav{width:200px;background:var(--side);padding:16px 8px;display:flex;flex-direction:column;gap:4px}
  nav button{text-align:left;padding:10px 12px;border:0;border-radius:8px;background:transparent;color:var(--fg);cursor:pointer;font-size:1em}
  nav button.on{background:var(--accent);color:#fff}
  main{flex:1;padding:24px 32px;overflow:auto}
</style>
