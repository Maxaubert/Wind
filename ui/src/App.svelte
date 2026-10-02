<script>
  import { onMount } from 'svelte';
  import { getMode, post } from './bridge.js';
  import Settings from './Settings.svelte';
  let mode = $state(getMode());
  // The wind-trails intro plays ONLY during onboarding (its Welcome step). Regular settings
  // launches open instantly - the splash-on-every-open was pure wait time.
  // Onboarding is only needed on first run, so it is a separate chunk loaded on demand.
  let Onboarding = $state(null);
  $effect(() => { if (mode === 'onboard' && !Onboarding) import('./Onboarding.svelte').then(m => { Onboarding = m.default; }); });
  // First-paint signal for the host's start-up measurement (two frames after mount).
  onMount(() => { requestAnimationFrame(() => requestAnimationFrame(() => post({ type: 'ready' }))); });
  function goToSettings() { mode = 'settings'; }
</script>
{#if mode === 'onboard'}
  {#if Onboarding}<Onboarding onDone={goToSettings} />{/if}
{:else}
  <Settings />
{/if}
