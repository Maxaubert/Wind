<script>
  import { onMount } from 'svelte';
  import { fade } from 'svelte/transition';
  import { getMode, getConfig } from './bridge.js';
  import { currentTheme, applyTheme } from './theme.js';
  import Settings from './Settings.svelte';
  import Onboarding from './Onboarding.svelte';
  import SplashIntro from './SplashIntro.svelte';
  let mode = getMode();
  // Splash on every settings launch. Onboarding already plays the same animation on its Welcome
  // step. Tests / mocks set window.__skipSplash so they do not have to wait for the intro.
  let showSplash = mode !== 'onboard' && !(typeof window !== 'undefined' && window.__skipSplash);
  onMount(async () => { const cfg = await getConfig(); applyTheme(currentTheme(cfg)); });
  function goToSettings() { mode = 'settings'; showSplash = false; }
</script>
{#if mode === 'onboard'}
  <Onboarding onDone={goToSettings} />
{:else}
  <Settings />
  {#if showSplash}
    <div class="splash-overlay" out:fade={{ duration: 260 }}>
      <SplashIntro onDone={() => showSplash = false} />
    </div>
  {/if}
{/if}
<style>
  .splash-overlay { position: fixed; inset: 0; z-index: 10; background: var(--bg); }
</style>
