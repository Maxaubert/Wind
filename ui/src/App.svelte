<script>
  import { onMount } from 'svelte';
  import { getMode, getConfig } from './bridge.js';
  import { currentTheme, applyTheme } from './theme.js';
  import Settings from './Settings.svelte';
  import Onboarding from './Onboarding.svelte';
  let mode = getMode();
  let theme = 'auto';
  // The wind-trails intro plays ONLY during onboarding (its Welcome step). Regular settings
  // launches open instantly - the splash-on-every-open was pure wait time.
  onMount(async () => { const cfg = await getConfig(); theme = currentTheme(cfg); applyTheme(theme); });
  function goToSettings() { mode = 'settings'; }
</script>
{#if mode === 'onboard'}
  <Onboarding {theme} onDone={goToSettings} />
{:else}
  <Settings />
{/if}
