<script>
  // Auto / Light / Dark as a radio group (arrows move, Tab leaves as one stop).
  import { iconSvg } from '../design/icons.js';
  let { value = 'auto', onChange = () => {}, labelledby, describedby } = $props();
  const modes = [['auto', 'Auto'], ['light', 'Light'], ['dark', 'Dark']];
  let el;
  function key(e) {
    const i = Math.max(0, modes.findIndex((m) => m[0] === value));
    let n = null;
    if (e.key === 'ArrowRight' || e.key === 'ArrowDown') n = (i + 1) % 3;
    else if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') n = (i + 2) % 3;
    if (n === null) return;
    e.preventDefault();
    onChange(modes[n][0]);
    setTimeout(() => el && el.querySelectorAll('button')[n]?.focus(), 0);
  }
</script>

<div class="th" bind:this={el} role="radiogroup" tabindex="-1" aria-labelledby={labelledby}
     aria-describedby={describedby} onkeydown={key}>
  {#each modes as [id, label] (id)}
    <button type="button" role="radio" aria-checked={value === id} tabindex={value === id ? 0 : -1}
            class:on={value === id} onclick={() => onChange(id)}>{@html iconSvg(id, 14)}{label}</button>
  {/each}
</div>

<style>
  .th { display: inline-flex; gap: 2px; padding: 3px; border-radius: var(--rad); background: var(--chip);
        border: 1px solid var(--chipb); }
  button { display: inline-flex; align-items: center; gap: 6px; height: 26px; padding: 0 12px;
           border-radius: var(--srad); color: var(--fg3); font: 13px var(--s); }
  button:hover:not(.on) { color: var(--fg); }
  button.on { background: var(--hlGrey); color: var(--fg); }
</style>
