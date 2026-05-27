<script>
  import { onMount } from 'svelte';
  import { getMode, getConfig } from './bridge.js';
  import { currentTheme, applyTheme } from './theme.js';
  import Settings from './Settings.svelte';
  import Onboarding from './Onboarding.svelte';
  let mode = getMode();
  onMount(async () => { const cfg = await getConfig(); applyTheme(currentTheme(cfg)); });
  function goToSettings() { mode = 'settings'; }
</script>
{#if mode === 'onboard'}
  <Onboarding onDone={goToSettings} />
{:else}
  <Settings />
{/if}
