<script>
  // Floating unsaved-changes pill. Renders nothing when there is nothing to save. It rises a few px
  // as it appears and eases back down when it goes. After a successful Save it stays for a moment
  // showing a "Saved" check, then eases away. The motion is a JS transition, so it checks
  // prefers-reduced-motion itself.
  import { cubicOut } from 'svelte/easing';
  let { count = 0, onSave = () => {}, onDiscard = () => {} } = $props();

  let last = 0;   // the count to show while the capsule is leaving (count is already 0 then)
  const n = $derived.by(() => { if (count > 0) last = count; return count > 0 ? count : last; });
  let hold = $state(false);   // keeps the pill up after a successful save
  let timer;
  const visible = $derived(count > 0 || hold);
  const saved = $derived(count === 0 && hold);

  async function save() {
    clearTimeout(timer);
    hold = true;   // set before the save lands, so the pill never drops out between "unsaved" and "Saved"
    const ok = await onSave();
    if (ok === false) hold = false;
    else timer = setTimeout(() => { hold = false; }, 700);
  }
  function discard() { clearTimeout(timer); hold = false; onDiscard(); }

  function rise(node) {
    const reduced = typeof matchMedia === 'function' && matchMedia('(prefers-reduced-motion: reduce)').matches;
    const ms = parseFloat(getComputedStyle(node).getPropertyValue('--dur')) || 180;
    return { duration: reduced ? 0 : ms, easing: cubicOut,
             css: (t) => `opacity:${t};transform:translateX(-50%) translateY(${(1 - t) * 6}px)` };
  }
</script>

{#if visible}
  <div class="capsule" role="status" transition:rise>
    {#if saved}
      <b class="ok"><svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true" focusable="false"><path d="M3 8.5 6.5 12 13 4.5"/></svg>Saved</b>
    {:else}
      <b>{n} unsaved {n === 1 ? 'change' : 'changes'}</b>
    {/if}
    <button type="button" class="btn g" disabled={saved} onclick={discard}>Discard</button>
    <button type="button" class="btn p" disabled={saved} onclick={save}>Save</button>
  </div>
{/if}

<style>
  /* centred on the main area (the 240px sidebar sits left of it), as in the mockup */
  .capsule { backdrop-filter: blur(14px); position: absolute; left: calc(50% + 120px); bottom: 26px; transform: translateX(-50%);
             height: 48px; border-radius: 999px; background: var(--pill); border: 1px solid var(--pillb);
             box-shadow: var(--shadow); display: flex; align-items: center; gap: 10px; padding: 0 5px 0 20px; white-space: nowrap; }
  b { font: 600 13px var(--s); }
  .ok { display: inline-flex; align-items: center; gap: 6px; }
  .ok svg { fill: none; stroke: currentColor; stroke-width: 1.75; stroke-linecap: round; stroke-linejoin: round;
            animation: tick var(--dur-fast) var(--ease); }
  @keyframes tick { from { opacity: 0; transform: scale(.8); } }
  .btn { height: 36px; padding: 0 20px; border-radius: 999px; font: 600 13px var(--s); }
  .btn:disabled { cursor: default; }
  .g { color: var(--fg2); }
  .p { background: var(--accent); color: var(--onaccent); }
  .g:hover:not(:disabled) { background: var(--hover); color: var(--fg); }
  .p:hover:not(:disabled) { opacity: .88; }
</style>
