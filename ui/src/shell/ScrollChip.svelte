<script>
  // The Settings page's scroll indicator (#329, Max 2026-10-02): one small fixed-size chip on the right
  // edge instead of a scrollbar. It shows only when the page has more to scroll, its position follows
  // the scroll, and it can be dragged. The page itself hides its native scrollbar (Settings.svelte).
  let { target } = $props();
  const CHIP = 36, PAD = 6;   // chip height and the gap it keeps from the top and bottom, px
  let show = $state(false), top = $state(0), dragging = $state(false);

  function update() {
    const el = target;
    if (!el) return;
    const max = el.scrollHeight - el.clientHeight;
    show = max > 1;
    if (!show) return;
    const track = el.clientHeight - PAD * 2 - CHIP;
    top = PAD + (el.scrollTop / max) * Math.max(0, track);
  }

  $effect(() => {
    const el = target;
    if (!el) return;
    update();
    el.addEventListener('scroll', update, { passive: true });
    const ro = new ResizeObserver(update);
    ro.observe(el);
    // Page switches, advanced rows and search results change the content height without a resize.
    const mo = new MutationObserver(update);
    mo.observe(el, { childList: true, subtree: true });
    return () => { el.removeEventListener('scroll', update); ro.disconnect(); mo.disconnect(); };
  });

  function down(e) {
    const el = target;
    if (!el) return;
    e.preventDefault();
    dragging = true;
    const startY = e.clientY, startTop = el.scrollTop;
    const max = el.scrollHeight - el.clientHeight;
    const track = Math.max(1, el.clientHeight - PAD * 2 - CHIP);
    const move = (ev) => { el.scrollTop = startTop + (ev.clientY - startY) * (max / track); };
    const up = () => {
      dragging = false;
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
  }
</script>

{#if show}
  <div class="chip" class:dragging style:top="{top}px" style:height="{CHIP}px" onpointerdown={down} aria-hidden="true"></div>
{/if}

<style>
  .chip { position: absolute; right: 4px; width: 4px; border-radius: 99px; z-index: 5;
          background: color-mix(in srgb, var(--fg) 22%, transparent);
          transition: width var(--dur-fast) var(--ease), right var(--dur-fast) var(--ease),
                      background-color var(--dur-fast) var(--ease); }
  .chip:hover, .chip.dragging { width: 6px; right: 3px; background: color-mix(in srgb, var(--fg) 36%, transparent); }
</style>
