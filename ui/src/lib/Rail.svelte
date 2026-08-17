<script>
  import { ic } from './icons.js';
  export let sections, active, onSelect, theme, onToggleTheme;
</script>
<aside class="rail">
  <div class="rail-glyph" aria-hidden="true">{@html ic.glyph}</div>
  <!-- A real navigation landmark (issue #201): this was a bare <div> of buttons, so a screen
       reader had no way to reach or identify the section switcher. aria-current marks the section
       being viewed - the CSS `active` class was the only signal before, i.e. sighted-only. -->
  <nav class="rail-nav" aria-label="Settings sections">
    {#each sections as s}
      <button class="ritem" type="button" class:active={s.id === active} title={s.label}
              aria-label={s.label} aria-current={s.id === active ? 'true' : undefined}
              on:click={() => onSelect(s.id)}>{@html ic[s.icon]}</button>
    {/each}
  </nav>
  <div class="rail-spacer"></div>
  <div class="rail-foot">
    <button class="ritem" type="button" title="Toggle theme"
            aria-label="Toggle theme (currently {theme})"
            on:click={onToggleTheme}>{@html theme === 'light' ? ic.moon : ic.sun}</button>
    <div class="avatar" title="Account (coming soon)" aria-hidden="true">{@html ic.person}</div>
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
