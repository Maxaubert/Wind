<script>
  export let row, value, onChange, disabled = false;
  export let values = {};   // consumed in Task 7 (keybind rows); accepted now as a no-op
  const num = v => Number(v);
</script>
<div class="row" class:disabled>
  <div class="meta"><div class="label">{row.label}</div>{#if row.desc}<div class="desc">{row.desc}</div>{/if}</div>
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
    {/if}
  </div>
</div>
<style>
  .row{display:flex;justify-content:space-between;align-items:center;gap:24px;padding:14px 0;border-bottom:1px solid var(--border)}
  .label{font-weight:600} .desc{font-size:.85em;color:var(--muted)} .val{margin-left:8px;min-width:3ch;display:inline-block}
  .row.disabled{opacity:.45}
</style>
