<script>
  export let id, label, desc = '';
</script>
<div class="sec" id={'sec-' + id}>
  <div class="sec-head"><h2>{label}</h2>{#if desc}<p>{desc}</p>{/if}</div>
  <slot />
</div>
<style>
  /* Ported from mockups/config-ui-onepage.html */
  .sec { padding-top: 6px; }
  .sec-head { position: sticky; top: 0; background: var(--bg); z-index: 2; padding: 16px 0 9px; }
  .sec-head h2 { margin: 0 0 4px; font-size: 24px; font-weight: 600; letter-spacing: -.2px; }
  .sec-head p { margin: 0; color: var(--muted); font-size: 13px; }
</style>
