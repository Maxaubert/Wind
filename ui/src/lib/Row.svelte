<script>
  import KeybindCapture from './KeybindCapture.svelte';
  import CustomSelect from './CustomSelect.svelte';
  import { pickExe } from '../bridge.js';
  import AppListModal from './AppListModal.svelte';
  export let row, value, onChange, disabled = false;
  export let values = {};   // keybind rows read sibling keys (e.g. zoomInVk) from here
  export let set = () => {}; // two-arg setter set(key, val); used by button rows
  export let live = () => {}; // live patch setter (writes setConfig immediately); used by keybind rows
  // Non-config state owned by Settings.svelte. The MPO row reflects a REGISTRY value, not an ini
  // key, so it must not travel through `values` - anything in there is diffed against `saved` and
  // written to the ini on Apply, which would put a junk key in magnifier.ini.
  export let extra = {};
  const num = v => Number(v);

  // ACCESSIBLE NAMING (issue #201). Every control in this file used to be anonymous: the label and
  // description are sibling <div>s, so a screen reader announced "checkbox, checked" / "slider, 1.2"
  // with no indication of WHICH setting it had landed on. The whole schema flows through this file,
  // so wiring the ids here names every row at once. Rules:
  //   - a control whose own text is NOT its name (checkbox, slider, colour) -> aria-labelledby=label
  //   - a control whose text is its VALUE (select trigger, keycap, Manage list) -> labelledby lists
  //     BOTH ids, so it reads "Magnifier model, Render" instead of losing one half or the other.
  // row.key is unique across the schema, and the '__' prefixed keys are valid id characters.
  $: rid = 'row-' + String(row.key).replace(/[^A-Za-z0-9_-]/g, '');
  $: labelId = row.label ? rid + '-l' : undefined;
  $: descId = row.desc ? rid + '-d' : undefined;
  // Value-bearing controls: name = "<label> <current value>". filter(Boolean) keeps this correct for
  // the label-less rows (outlineColor has no desc, __about has neither).
  $: valueId = rid + '-v';
  $: labelledByWithValue = [labelId, valueId].filter(Boolean).join(' ');
  // Sliders read as a bare number without this ("Smooth ramp (s)" -> "0.6"). row.unit carries the
  // spoken unit; rows without one keep the plain number.
  $: valueText = row.unit ? `${value} ${row.unit}` : undefined;

  // 'applist' rows hold a comma-separated exe list in one config string (the core parses it with
  // IsExeInList). Split for display, re-join on every edit, so the stored shape never changes.
  $: items = String(value ?? '').split(',').map(s => s.trim()).filter(Boolean);
  const joined = arr => arr.join(',');
  let listOpen = false;
  // Keep the configured state visible on the row itself; a bare "Manage list" button would hide
  // whether anything is set at all. One entry is worth naming, more than that only worth counting.
  $: summary = items.length === 0 ? 'None' : items.length === 1 ? items[0] : `${items.length} apps`;
  async function addApp() {
    const name = await pickExe();                       // '' when the user cancels
    if (!name) return;
    // Case-insensitive dedupe: the core matches case-insensitively, so two spellings of one exe
    // would both "work" while looking like a broken list.
    if (items.some(i => i.toLowerCase() === name.toLowerCase())) return;
    onChange(joined([...items, name]));
  }
  function removeApp(name) {
    onChange(joined(items.filter(i => i !== name)));
  }
  // Segmented control = a radio group, so arrows move the selection (the ARIA radiogroup contract)
  // while Tab enters and leaves it as a single stop. Without this the two options were plain
  // buttons and nothing exposed which one was active.
  let segEl;
  // Which radio carries tabindex=0. Normally the selected one, but a value outside the option range
  // (an unset or junk ini key) would otherwise leave EVERY radio at -1, making the whole group
  // unreachable by keyboard - so it falls back to the first.
  $: segFocusIdx = row.seg && num(value) >= 0 && num(value) < row.seg.length ? num(value) : 0;
  function segKey(e) {
    if (disabled) return;
    const n = row.seg.length;
    const cur = num(value);
    let next = null;
    if (e.key === 'ArrowRight' || e.key === 'ArrowDown') next = (cur + 1) % n;
    else if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') next = (cur - 1 + n) % n;
    else if (e.key === 'Home') next = 0;
    else if (e.key === 'End') next = n - 1;
    if (next === null) return;
    e.preventDefault();
    onChange(next);
    // Focus follows selection in a radio group; the button does not exist until Svelte re-renders.
    setTimeout(() => segEl && segEl.querySelectorAll('button')[next]?.focus(), 0);
  }
</script>
{#if row.type === 'about'}
  <div class="about-hero">
    <svg class="logo" viewBox="0 0 16 16" width="96" height="96" fill="none" stroke="currentColor"
         stroke-width="1.5" stroke-linecap="round" aria-hidden="true" focusable="false">
      <path d="M2 5.5h8.5a2 2 0 1 0-2-2"/>
      <path d="M2 9h11a2 2 0 1 1-2 2"/>
      <path d="M2 12.5h6.5a1.7 1.7 0 1 1-1.7 1.7"/>
    </svg>
    <div class="name">Wind</div>
    <p class="tag">Barely there. Everywhere.</p>
    <p class="version">v0.9.0</p>
    <a class="link" href="https://github.com/Maxaubert/Wind" target="_blank" rel="noopener">View on GitHub</a>
  </div>
{:else}
  <div class="row" class:disabled>
    <div class="meta">{#if row.label}<div class="label" id={labelId}>{row.label}</div>{/if}{#if row.desc}<div class="desc" id={descId}>{row.desc}</div>{/if}</div>
    <div class="ctl">
      {#if row.type === 'toggle'}
        <label class="checkbox-wrapper" class:disabled>
          <input type="checkbox" {disabled} checked={num(value) === 1}
                 aria-labelledby={labelId} aria-describedby={descId}
                 on:change={e => onChange(e.target.checked ? 1 : 0)} />
          <svg viewBox="0 0 35.6 35.6" aria-hidden="true" focusable="false">
            <circle class="background" cx="17.8" cy="17.8" r="17.8"></circle>
            <circle class="stroke" cx="17.8" cy="17.8" r="14.37"></circle>
            <polyline class="check" points="11.78 18.12 15.55 22.23 25.17 12.87"></polyline>
          </svg>
        </label>
      {:else if row.type === 'slider'}
        <input type="range" {disabled} min={row.min} max={row.max} step={row.step} value={value}
               aria-labelledby={labelId} aria-describedby={descId} aria-valuetext={valueText}
               on:input={e => onChange(e.target.value)} />
        <span class="val" aria-hidden="true">{value}</span>
      {:else if row.type === 'select'}
        <CustomSelect {value} options={row.options} labels={row.optionLabels} {disabled} onChange={onChange}
                      labelledby={labelledByWithValue} describedby={descId} {valueId} />
      {:else if row.type === 'keybind'}
        <KeybindCapture {row} {values} onChange={live} {disabled}
                        labelledby={labelledByWithValue} describedby={descId} {valueId} />
      {:else if row.type === 'button'}
        <button class="linkbtn" type="button" {disabled} id={valueId}
                aria-labelledby={labelledByWithValue} aria-describedby={descId}
                on:click={() => set('__action', row.action)}>{row.btn}</button>
      {:else if row.type === 'segmented'}
        <!-- tabindex="-1" on the group itself: the roving tabindex lives on the radios, so the
             group is one Tab stop and arrows move within it. -->
        <div class="seg" class:disabled bind:this={segEl} role="radiogroup" tabindex="-1"
             aria-labelledby={labelId} aria-describedby={descId} on:keydown={segKey}>
          {#each row.seg as opt, i}
            <button class="seg-opt" type="button" class:active={num(value) === i} {disabled}
                    role="radio" aria-checked={num(value) === i} tabindex={i === segFocusIdx ? 0 : -1}
                    on:click={() => onChange(i)}>{opt}</button>
          {/each}
        </div>
      {:else if row.type === 'color'}
        <input class="color" type="color" {disabled} value={value}
               aria-labelledby={labelId} aria-describedby={descId}
               on:input={e => onChange(e.target.value)} />
      {:else if row.type === 'applist'}
        <span class="listsummary" id={valueId}>{summary}</span>
        <!-- Name is "<row label>, <summary>, Manage list": which list, what is in it, what the
             button does. The bare "Manage list" repeated on every applist row otherwise. -->
        <button class="linkbtn" type="button" {disabled} id={rid + '-b'}
                aria-labelledby="{labelledByWithValue} {rid}-b" aria-describedby={descId}
                on:click={() => (listOpen = true)}>Manage list</button>
      {:else if row.type === 'mpo'}
        <!-- The toggle IS the detector: unticked means MPO is on. An extra "MPO on" badge beside it
             said the same thing twice and made the row read as an action button rather than a state,
             so the only chip here is the staged "Requires restart" note. Flex-wrapped because an
             empty inline-block checkbox baselines at its bottom edge, which sat the chip visibly
             below the toggle. -->
        <div class="mpoctl">
          <!-- Driven by staged-vs-BOOT, not staged-vs-registry: the chip answers "will a restart
               change anything", which is also why it can show with nothing staged (the registry
               already holds a value DWM has not loaded yet). -->
          {#if extra.mpoNeedsRestart}
            <span class="tag" id={rid + '-t'}>Requires restart</span>
          {/if}
          <label class="checkbox-wrapper" class:disabled={disabled || !extra.mpoKnown}>
            <!-- The "Requires restart" chip joins the description when it is showing: it is the
                 only signal that an applied change has not taken effect yet, and sighted users
                 read it right next to the toggle. -->
            <input type="checkbox" disabled={disabled || !extra.mpoKnown} checked={!!extra.mpoStaged}
                   aria-labelledby={labelId}
                   aria-describedby={extra.mpoNeedsRestart ? `${descId ?? ''} ${rid}-t`.trim() : descId}
                   on:change={e => set('__mpoStaged', e.target.checked)} />
            <svg viewBox="0 0 35.6 35.6" aria-hidden="true" focusable="false">
              <circle class="background" cx="17.8" cy="17.8" r="17.8"></circle>
              <circle class="stroke" cx="17.8" cy="17.8" r="14.37"></circle>
              <polyline class="check" points="11.78 18.12 15.55 22.23 25.17 12.87"></polyline>
            </svg>
          </label>
        </div>
      {/if}
    </div>
  </div>
  {#if listOpen}
    <AppListModal title={row.label} {items} onAdd={addApp} onRemove={removeApp}
                  onClose={() => (listOpen = false)} />
  {/if}
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
  /* The real checkbox is opacity:0 on top of the SVG, so its own focus ring is invisible too.
     Put the ring on the wrapper instead. :has(:focus-visible) (not :focus-within) so a mouse
     click does not leave a ring behind. */
  .checkbox-wrapper:has(input[type=checkbox]:focus-visible){outline:2px solid var(--focus);
    outline-offset:3px;border-radius:50%}
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
  /* App list: one chip per exe plus a "+" that opens the host's file picker. Wraps and is
     right-aligned so a growing list pushes downward instead of squeezing the .meta column. */
  /* Current state beside the Manage button. nowrap + a max width so a long exe name truncates
     instead of wrapping the control onto a second line (which is what the old inline chips did). */
  .listsummary{color:var(--muted);font-size:12px;margin-right:10px;max-width:180px;
               display:inline-block;vertical-align:middle;overflow:hidden;text-overflow:ellipsis;
               white-space:nowrap}
  /* MPO row: flex so the "Requires restart" chip centres on the TOGGLE rather than baselining
     against the row's description text. */
  .mpoctl{display:flex;align-items:center;gap:10px}
  .tag{background:var(--chip);color:var(--muted);border:1px solid var(--line);border-radius:999px;
       padding:3px 9px;font-size:11px;line-height:1.4;white-space:nowrap}
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
