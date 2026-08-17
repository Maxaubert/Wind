<script>
  export let id, label, desc = '';
</script>
<!-- A real <section> named by its heading, so screen readers can jump section to section instead of
     walking every row. The h2 takes tabindex="-1" because the rail moves focus to it on activate
     (issue #201): the rail used to scroll the pane and leave focus behind, which told a keyboard
     or screen-reader user nothing. -1 keeps it out of the Tab sequence. -->
<section class="sec" id={'sec-' + id} aria-labelledby={'sech-' + id}>
  <div class="sec-head"><h2 id={'sech-' + id} tabindex="-1">{label}</h2>{#if desc}<p>{desc}</p>{/if}</div>
  <slot />
</section>
<style>
  /* Ported from mockups/config-ui-onepage.html */
  .sec { padding-top: 6px; display: block; }
  .sec-head { position: sticky; top: 0; background: var(--bg); z-index: 2; padding: 16px 0 9px; }
  .sec-head h2 { margin: 0 0 4px; font-size: 24px; font-weight: 600; letter-spacing: -.2px; }
  /* Focused only programmatically (tabindex=-1), and the scroll + the announcement already show
     where you are, so the ring would just be visual noise on a click. */
  .sec-head h2:focus { outline: none; }
  .sec-head h2:focus-visible { outline: 2px solid var(--focus); outline-offset: 3px; }
  .sec-head p { margin: 0; color: var(--muted); font-size: 13px; }
</style>
