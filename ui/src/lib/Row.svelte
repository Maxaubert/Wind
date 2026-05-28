<script>
  import KeybindCapture from './KeybindCapture.svelte';
  export let row, value, onChange, disabled = false;
  export let values = {};   // keybind rows read sibling keys (e.g. zoomInVk) from here
  export let set = () => {}; // two-arg setter set(key, val); used by keybind/button rows
  const num = v => Number(v);
</script>
<div class="row" class:disabled>
  <div class="meta">{#if row.label}<div class="label">{row.label}</div>{/if}{#if row.desc}<div class="desc">{row.desc}</div>{/if}</div>
  <div class="ctl">
    {#if row.type === 'toggle'}
      <input type="checkbox" {disabled} checked={num(value) === 1} on:change={e => onChange(e.target.checked ? 1 : 0)} />
    {:else if row.type === 'slider'}
      <input type="range" {disabled} min={row.min} max={row.max} step={row.step} value={value} on:input={e => onChange(e.target.value)} />
      <span class="val">{value}</span>
    {:else if row.type === 'select'}
      <select {disabled} value={value} on:change={e => onChange(e.target.value)}>
        {#each row.options as o}<option value={o}>{o}</option>{/each}
      </select>
    {:else if row.type === 'keybind'}
      <KeybindCapture {row} {values} onChange={set} {disabled} />
    {:else if row.type === 'button'}
      <button class="linkbtn" {disabled} on:click={() => set('__action', row.action)}>{row.btn}</button>
    {:else if row.type === 'about'}
      <div class="about">Wind, a fast magnifier. <a href="https://github.com/Maxaubert/Wind" target="_blank" rel="noopener">GitHub</a></div>
    {/if}
  </div>
</div>
<style>
  .row{display:flex;justify-content:space-between;align-items:center;gap:24px;padding:14px 0;border-bottom:1px solid var(--line)}
  .label{font-weight:600} .desc{font-size:.85em;color:var(--muted)} .val{margin-left:8px;min-width:3ch;display:inline-block}
  .row.disabled{opacity:.45}
  /* .linkbtn ported from mockups/config-ui-onboarding.html. */
  .linkbtn{padding:5px 11px;border-radius:7px;border:1px solid var(--line);background:transparent;font-size:12px;color:var(--text);cursor:pointer}
  .linkbtn:disabled{opacity:.5;cursor:default}
  .about{color:var(--muted)} .about a{color:var(--accent);text-decoration:none} .about a:hover{text-decoration:underline}
</style>
