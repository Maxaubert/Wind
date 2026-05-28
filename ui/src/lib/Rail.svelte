<script>
  import { ic } from './icons.js';
  export let sections, active, onSelect, theme, onToggleTheme;
</script>
<aside class="rail">
  <div class="rail-glyph" title="Wind">{@html ic.glyph}</div>
  <div class="rail-nav">
    {#each sections as s}
      <button class="ritem" class:active={s.id === active} title={s.label} aria-label={s.label}
              on:click={() => onSelect(s.id)}>{@html ic[s.icon]}</button>
    {/each}
  </div>
  <div class="rail-spacer"></div>
  <div class="rail-foot">
    <button class="ritem" title="Toggle theme" aria-label="Toggle theme" on:click={onToggleTheme}>{@html theme === 'light' ? ic.moon : ic.sun}</button>
    <div class="avatar" title="Account (coming soon)">{@html ic.person}</div>
  </div>
</aside>
<style>
  /* Ported from mockups/config-ui-onepage.html */
  .rail { width: 64px; flex-shrink: 0; background: var(--rail); display: flex; flex-direction: column;
          align-items: center; padding: 12px 0 14px; border-right: 1px solid var(--panel-line); }
  .rail-glyph { width: 40px; height: 40px; display: grid; place-items: center; color: var(--accent-icon); margin-bottom: 10px; }
  .rail-nav { display: flex; flex-direction: column; gap: 6px; align-items: center; }
  .ritem { width: 42px; height: 42px; border-radius: 10px; display: grid; place-items: center;
           color: var(--muted); cursor: pointer; position: relative; border: 0; background: transparent; }
  .ritem:hover { background: var(--hover); color: var(--text); }
  .ritem.active { background: var(--accent-soft); color: var(--accent-icon); }
  .ritem.active::before { content: ''; position: absolute; left: -12px; top: 11px; bottom: 11px;
                          width: 3px; border-radius: 2px; background: var(--accent); }
  .rail-spacer { flex: 1; }
  .rail-foot { display: flex; flex-direction: column; gap: 8px; align-items: center; }
  .avatar { width: 30px; height: 30px; border-radius: 50%; background: var(--hover); color: var(--muted);
            display: grid; place-items: center; }
</style>
