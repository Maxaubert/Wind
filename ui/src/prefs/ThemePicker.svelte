<script>
  // Theme picker (mockup option A, Max 2026-10-02): ONE row of mini window preview cards with the theme name
  // under each and the selected card outlined. There are four themes, so they always fit: no scrolling, no
  // arrow buttons, no edge fades. Left/Right (and Up/Down, Home, End) move the selection and apply it.
  // Each swatch is a tiny window drawn with that theme's real tokens: it is its own .wnd element carrying
  // data-palette and data-theme, so themes.css colours it with no colour values duplicated in JS.
  //   value     the palette id (uiPalette)      mode  the RESOLVED mode, dark or light
  //   onChange  onChange(id)
  import { themes } from '../design/themes.js';
  let { value = 'grey', mode = 'dark', onChange = () => {}, labelledby, describedby } = $props();
  let el;
  function key(e) {
    const i = Math.max(0, themes.findIndex((t) => t.id === value));
    let n = null;
    if (e.key === 'ArrowRight' || e.key === 'ArrowDown') n = (i + 1) % themes.length;
    else if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') n = (i + themes.length - 1) % themes.length;
    else if (e.key === 'Home') n = 0;
    else if (e.key === 'End') n = themes.length - 1;
    if (n === null) return;
    e.preventDefault();
    onChange(themes[n].id);
    setTimeout(() => { const b = el && el.querySelectorAll('button')[n]; if (b) b.focus(); }, 0);
  }
</script>

<div class="strip" bind:this={el} role="radiogroup" tabindex="-1" aria-labelledby={labelledby} aria-describedby={describedby}
     onkeydown={key}>
  {#each themes as t (t.id)}
    <button type="button" role="radio" class="th" class:on={value === t.id} aria-checked={value === t.id}
            tabindex={value === t.id ? 0 : -1} data-palette-id={t.id} onclick={() => onChange(t.id)}>
      <span class="wnd sw" data-palette={t.id} data-theme={mode} aria-hidden="true">
        <i class="sb"></i><i class="bn"></i><i class="r1"></i><i class="r2"></i><i class="ac"></i>
      </span>
      <span class="nm">{t.label}</span>
    </button>
  {/each}
</div>

<style>
  /* One row, never wraps, never scrolls: four cards of 106px (they shrink a little before anything overflows). */
  .strip { display: flex; flex-wrap: nowrap; justify-content: flex-end; gap: 6px; max-width: 100%; padding: 3px 0; }
  .th { flex: 0 1 auto; width: 106px; min-width: 84px; padding: 4px; border: 1px solid transparent; border-radius: var(--rc); text-align: left;
        color: var(--fg3); }
  .th:hover { border-color: var(--chipb); color: var(--fg); }
  .th.on { border-color: var(--fg); box-shadow: inset 0 0 0 1px var(--fg); color: var(--fg); }
  .th:focus-visible { outline-offset: 2px; }
  .nm { display: block; margin: 5px 2px 1px; font: 11px var(--m); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }

  /* The mini window. It is a .wnd of its own, so these colours are that theme's tokens. */
  .sw { display: block; position: relative; height: 52px; border-radius: var(--srad); background: var(--bg);
        border: 1px solid var(--line2); overflow: hidden; }
  .sw i { position: absolute; display: block; }
  .sb { left: 0; top: 0; bottom: 0; width: 20px; background: color-mix(in srgb, var(--fill) 7%, var(--side));
        border-right: 1px solid var(--line2); }
  .sb::before { content: ""; position: absolute; left: 4px; right: 4px; top: 7px; height: 5px; border-radius: var(--srad); background: var(--sel); }
  .bn { left: 20px; right: 0; top: 0; height: 12px;
        background: linear-gradient(90deg, var(--band) 30%, color-mix(in srgb, var(--bntint) 55%, var(--band))); }
  .r1, .r2 { left: 26px; right: 6px; height: 8px; border-radius: var(--srad); background: var(--hover); border: 1px solid var(--cardb); }
  .r1 { top: 18px; }
  .r2 { top: 29px; }
  .ac { left: 30px; width: 30px; top: 43px; height: 3px; border-radius: 2px; background: var(--fill); box-shadow: 0 0 0 1px var(--fillline); }
</style>
