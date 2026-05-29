<script>
  import KeybindCapture from './KeybindCapture.svelte';
  import CustomSelect from './CustomSelect.svelte';
  export let row, value, onChange, disabled = false;
  export let values = {};   // keybind rows read sibling keys (e.g. zoomInVk) from here
  export let set = () => {}; // two-arg setter set(key, val); used by button rows
  export let live = () => {}; // live patch setter (writes setConfig immediately); used by keybind rows
  const num = v => Number(v);
</script>
{#if row.type === 'about'}
  <div class="about-hero">
    <svg class="logo" viewBox="0 0 16 16" width="96" height="96" fill="none" stroke="currentColor"
         stroke-width="1.5" stroke-linecap="round">
      <path d="M2 5.5h8.5a2 2 0 1 0-2-2"/>
      <path d="M2 9h11a2 2 0 1 1-2 2"/>
      <path d="M2 12.5h6.5a1.7 1.7 0 1 1-1.7 1.7"/>
    </svg>
    <div class="name">Wind</div>
    <p class="tag">A fast magnifier that lives in your tray.</p>
    <p class="version">v0.9.0</p>
    <a class="link" href="https://github.com/Maxaubert/Wind" target="_blank" rel="noopener">View on GitHub</a>
  </div>
{:else}
  <div class="row" class:disabled>
    <div class="meta">{#if row.label}<div class="label">{row.label}</div>{/if}{#if row.desc}<div class="desc">{row.desc}</div>{/if}</div>
    <div class="ctl">
      {#if row.type === 'toggle'}
        <input type="checkbox" {disabled} checked={num(value) === 1} on:change={e => onChange(e.target.checked ? 1 : 0)} />
      {:else if row.type === 'slider'}
        <input type="range" {disabled} min={row.min} max={row.max} step={row.step} value={value} on:input={e => onChange(e.target.value)} />
        <span class="val">{value}</span>
      {:else if row.type === 'select'}
        <CustomSelect {value} options={row.options} {disabled} onChange={onChange} />
      {:else if row.type === 'keybind'}
        <KeybindCapture {row} {values} onChange={live} {disabled} />
      {:else if row.type === 'button'}
        <button class="linkbtn" {disabled} on:click={() => set('__action', row.action)}>{row.btn}</button>
      {/if}
    </div>
  </div>
{/if}
<style>
  .row{display:flex;justify-content:space-between;align-items:center;gap:24px;padding:14px 0;border-bottom:1px solid var(--line)}
  .label{font-weight:600} .desc{font-size:.85em;color:var(--muted)}
  /* Fixed width + tabular-nums + right-align so the readout doesn't widen as digits change
     (1 -> 10 etc.), which would otherwise reflow the .ctl and visually shake the slider. */
  .val{margin-left:8px;width:4ch;display:inline-block;text-align:right;font-variant-numeric:tabular-nums}
  .row.disabled{opacity:.45}
  /* .linkbtn ported from mockups/config-ui-onboarding.html. */
  .linkbtn{padding:5px 11px;border-radius:7px;border:1px solid var(--line);background:transparent;font-size:12px;color:var(--text);cursor:pointer}
  .linkbtn:disabled{opacity:.5;cursor:default}
  /* About hero: large centered Wind logo fills the section so it has real height (helps the
     scroll-spy reach About) and the bottom of the scroll area isn't empty. */
  .about-hero{padding:48px 0 64px;text-align:center;color:var(--text);display:flex;flex-direction:column;align-items:center}
  /* overflow:visible lets the bottom curl-arc render past the 16-unit viewBox (the stroke
     extends ~1 unit below otherwise gets clipped, looked like 2-3px shaved off the bottom). */
  .about-hero .logo{color:var(--accent-icon);margin:0 0 18px;overflow:visible}
  .about-hero .name{font-size:30px;font-weight:700;letter-spacing:-.4px;margin-bottom:6px}
  .about-hero .tag{color:var(--muted);margin:0 0 4px;font-size:14px}
  .about-hero .version{color:var(--muted);font-size:12.5px;margin:0 0 22px}
  .about-hero .link{color:var(--accent);text-decoration:none;font-size:13px;padding:8px 16px;border:1px solid var(--line);border-radius:7px;display:inline-block}
  .about-hero .link:hover{background:var(--hover)}
</style>
