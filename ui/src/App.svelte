<script>
  import { onMount } from 'svelte';
  import { getMode, getConfig, post } from './bridge.js';
  import { currentTheme, applyTheme } from './theme.js';
  import Settings from './Settings.svelte';
  let mode = $state(getMode());
  let theme = $state('auto');
  // The wind-trails intro plays ONLY during onboarding (its Welcome step). Regular settings
  // launches open instantly - the splash-on-every-open was pure wait time.
  // Onboarding is only needed on first run, so it is a separate chunk loaded on demand.
  let Onboarding = $state(null);
  $effect(() => { if (mode === 'onboard' && !Onboarding) import('./Onboarding.svelte').then(m => { Onboarding = m.default; }); });
  // First-paint signal for the host's start-up measurement (two frames after mount).
  onMount(() => { requestAnimationFrame(() => requestAnimationFrame(() => post({ type: 'ready' }))); });
  onMount(async () => { const cfg = await getConfig(); theme = currentTheme(cfg); applyTheme(theme); });
  function goToSettings() { mode = 'settings'; }
</script>
{#if mode === 'onboard'}
  {#if Onboarding}<Onboarding {theme} onDone={goToSettings} />{/if}
{:else}
  <Settings />
{/if}
