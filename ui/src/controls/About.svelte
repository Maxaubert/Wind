<script>
  // About page body: logo, name, tagline, version and the GitHub link (today's About content).
  import { iconSvg } from '../design/icons.js';
  let { version = '', onRepo = () => {} } = $props();
</script>

<div class="about">
  <div class="logo">{@html iconSvg('logo', 72)}</div>
  <div class="name">Wind</div>
  <p class="tag">Barely there. Everywhere.</p>
  {#if version}<p class="ver">v{version}</p>{/if}
  <button type="button" class="link" onclick={onRepo}>Star on GitHub</button>
  <p class="ask">A star helps others find Wind</p>
</div>

<style>
  .about { padding: 40px 0 48px; text-align: center; display: flex; flex-direction: column; align-items: center; }
  .logo { color: var(--fg); margin-bottom: 14px; }
  .logo :global(svg) { stroke-width: 1.1; }
  .name { font: 600 28px var(--s); letter-spacing: -.3px; margin-bottom: 4px; }
  .tag { color: var(--fg2); margin: 0 0 4px; font-size: 14px; }
  .ver { color: var(--fg3); font: 12.5px var(--m); margin: 0 0 20px; }
  .link { height: 32px; padding: 0 16px; border-radius: var(--srad); background: var(--chip);
          border: 1px solid var(--chipb); color: var(--fg); font: 600 13px var(--s); }
  .link:hover { border-color: var(--outline); }
  .ask { color: var(--fg3); font-size: 12.5px; margin: 12px 0 0; }
</style>
