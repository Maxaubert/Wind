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
        <label class="checkbox-wrapper" class:disabled>
          <input type="checkbox" {disabled} checked={num(value) === 1} on:change={e => onChange(e.target.checked ? 1 : 0)} />
          <svg viewBox="0 0 35.6 35.6" aria-hidden="true">
            <circle class="background" cx="17.8" cy="17.8" r="17.8"></circle>
            <circle class="stroke" cx="17.8" cy="17.8" r="14.37"></circle>
            <polyline class="check" points="11.78 18.12 15.55 22.23 25.17 12.87"></polyline>
          </svg>
        </label>
      {:else if row.type === 'slider'}
        <input type="range" {disabled} min={row.min} max={row.max} step={row.step} value={value} on:input={e => onChange(e.target.value)} />
        <span class="val">{value}</span>
      {:else if row.type === 'select'}
        <CustomSelect {value} options={row.options} {disabled} onChange={onChange} />
      {:else if row.type === 'keybind'}
        <KeybindCapture {row} {values} onChange={live} {disabled} />
      {:else if row.type === 'button'}
        <button class="linkbtn" {disabled} on:click={() => set('__action', row.action)}>{row.btn}</button>
      {:else if row.type === 'segmented'}
        <div class="seg" class:disabled>
          {#each row.seg as opt, i}
            <button class="seg-opt" class:active={num(value) === i} {disabled}
                    on:click={() => onChange(i)}>{opt}</button>
          {/each}
        </div>
      {:else if row.type === 'color'}
        <input class="color" type="color" {disabled} value={value}
               on:input={e => onChange(e.target.value)} />
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
  /* Segmented pill switch (two labeled options): a subtle dark pill with a lighter grey knob over
     the active option. Sleek/neutral to match the UI (no accent fill). */
  .seg{display:inline-flex;background:var(--chip);border:1px solid var(--line);border-radius:999px;padding:3px;gap:2px}
  .seg-opt{padding:6px 16px;border:0;background:transparent;color:var(--muted);font-size:12.5px;
           font-weight:600;cursor:pointer;border-radius:999px;transition:background .15s,color .15s}
  .seg-opt:hover:not(.active){color:var(--text)}
  .seg-opt.active{background:var(--track);color:var(--text)}
  .seg.disabled{opacity:.45}
  .seg-opt:disabled{cursor:default}
  /* Animated SVG checkbox: a circular knob whose ring + check draw on when checked. Dark-grey
     fill (replacing the original purple) to fit the UI; white ring/check. */
  .checkbox-wrapper{position:relative;display:inline-block;width:26px;height:26px}
  .checkbox-wrapper.disabled{pointer-events:none}
  .checkbox-wrapper .background{fill:#3a3a44;transition:ease all .6s}
  .checkbox-wrapper .stroke{fill:none;stroke:#fff;stroke-miterlimit:10;stroke-width:2px;
    stroke-dashoffset:100;stroke-dasharray:100;transition:ease all .6s}
  .checkbox-wrapper .check{fill:none;stroke:#fff;stroke-linecap:round;stroke-linejoin:round;
    stroke-width:2px;stroke-dashoffset:22;stroke-dasharray:22;transition:ease all .6s}
  .checkbox-wrapper input[type=checkbox]{position:absolute;width:100%;height:100%;left:0;top:0;
    margin:0;opacity:0;-webkit-appearance:none;appearance:none}
  .checkbox-wrapper input[type=checkbox]:hover{cursor:pointer}
  .checkbox-wrapper:hover .check{stroke-dashoffset:0}
  .checkbox-wrapper input[type=checkbox]:checked + svg .background{fill:var(--accent)}
  .checkbox-wrapper input[type=checkbox]:checked + svg .stroke{stroke-dashoffset:0}
  .checkbox-wrapper input[type=checkbox]:checked + svg .check{stroke-dashoffset:0}
  /* .linkbtn ported from mockups/config-ui-onboarding.html. */
  .linkbtn{padding:5px 11px;border-radius:7px;border:1px solid var(--line);background:transparent;font-size:12px;color:var(--text);cursor:pointer}
  .linkbtn:disabled{opacity:.5;cursor:default}
  .color{width:42px;height:26px;padding:2px;border:1px solid var(--line);border-radius:7px;
         background:transparent;cursor:pointer}
  .color:disabled{cursor:default}   /* .row.disabled already dims the whole row; avoid compounding opacity */
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
