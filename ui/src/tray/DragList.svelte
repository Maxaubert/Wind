<script>
  // A card of reorderable rows. Reusable: the caller owns the order and renders each row's content.
  //   items       [{ key, ... }] in display order (key is unique)
  //   onReorder   onReorder(keys) with the new full order
  //   onRowClick  onRowClick(item) when a row is clicked (not dragged); the grip never counts
  //   announce    announce(text) for the live region (pick up, move, drop)
  //   row         snippet (item) rendering the row body: the grid cells after the grip
  //   nameOf      item -> spoken name
  // Mouse or touch: press a row and drag; the picked row lifts and the others glide ~150 ms.
  // Keyboard: focus a grip, Space or Enter picks up, Up/Down move, Space or Enter drops, Escape
  // cancels (the order goes back), losing focus drops. Rows only reorder within their own list.
  import { tick } from 'svelte';
  let { items = [], label = '', labelledby = undefined, onReorder = () => {}, onRowClick = () => {},
        announce = () => {}, nameOf = (it) => it.name, row } = $props();

  let list = $state();
  let pickedKey = $state(null);   // keyboard pick-up
  let drag = null;                // pointer drag in flight
  let kb = null;                  // { key, from, order } while picked up
  let moving = false;             // true while a keyboard move re-renders (focus bounces)
  let justDragged = false;

  const GRIP = '<svg viewBox="0 0 16 16" aria-hidden="true"><rect x="4" y="3" width="2.5" height="2.5"/><rect x="9.5" y="3" width="2.5" height="2.5"/><rect x="4" y="6.75" width="2.5" height="2.5"/><rect x="9.5" y="6.75" width="2.5" height="2.5"/><rect x="4" y="10.5" width="2.5" height="2.5"/><rect x="9.5" y="10.5" width="2.5" height="2.5"/></svg>';

  const rows = () => [...list.querySelectorAll(':scope > .trow')];
  const keys = () => items.map((i) => i.key);
  const byKey = (k) => items.find((i) => i.key === k);
  const scroller = () => {
    for (let e = list.parentElement; e; e = e.parentElement) {
      const o = getComputedStyle(e).overflowY;
      if (o === 'auto' || o === 'scroll') return e;
    }
    return document.scrollingElement;
  };

  // ---- pointer drag ---------------------------------------------------------------------------
  function layout() {
    const d = drag, n = d.rows.length, st = d.step;
    const dy = Math.max(-d.idx * st, Math.min((n - 1 - d.idx) * st, d.lastY - d.startY + (d.sc.scrollTop - d.startScroll)));
    d.row.style.transform = 'translateY(' + dy + 'px)';
    d.target = Math.max(0, Math.min(n - 1, Math.round(d.idx + dy / st)));
    d.rows.forEach((r, j) => {
      if (r === d.row) return;
      let s = 0;
      if (d.idx < d.target && j > d.idx && j <= d.target) s = -st;
      else if (d.target < d.idx && j >= d.target && j < d.idx) s = st;
      r.style.transform = s ? 'translateY(' + s + 'px)' : '';
    });
  }
  function autoScroll() {
    const d = drag;
    if (!d || !d.active) return;
    const rc = d.sc.getBoundingClientRect(), z = 56;
    let v = 0;
    if (d.lastY < rc.top + z) v = -Math.ceil((rc.top + z - d.lastY) / 4);
    else if (d.lastY > rc.bottom - z) v = Math.ceil((d.lastY - (rc.bottom - z)) / 4);
    if (v) {
      const before = d.sc.scrollTop;
      d.sc.scrollTop += Math.max(-14, Math.min(14, v));
      if (d.sc.scrollTop !== before) layout();
    }
    d.raf = requestAnimationFrame(autoScroll);
  }
  function onPointerDown(e) {
    if (e.button !== 0 || drag || e.target.closest('.chk')) return;
    const rowEl = e.target.closest('.trow');
    if (!rowEl) return;
    startDrag(e, rowEl);
  }
  function startDrag(e, rowEl) {
    const r = rows();
    drag = { row: rowEl, rows: r, idx: r.indexOf(rowEl), target: r.indexOf(rowEl), startY: e.clientY, lastY: e.clientY,
             sc: scroller(), step: r.length > 1 ? r[1].offsetTop - r[0].offsetTop : 62, active: false, id: e.pointerId };
    drag.startScroll = drag.sc.scrollTop;
  }
  function onPointerMove(e) {
    const d = drag;
    if (!d || e.pointerId !== d.id) return;
    d.lastY = e.clientY;
    if (!d.active) {
      if (Math.abs(e.clientY - d.startY) < 4) return;
      d.active = true;
      try { d.row.setPointerCapture(e.pointerId); } catch (_) { /* synthetic pointers */ }
      d.rows.forEach((r) => { if (r !== d.row) r.classList.add('shift'); });
      d.row.classList.add('dragging');
      d.raf = requestAnimationFrame(autoScroll);
    }
    layout();
  }
  function finish(cancel) {
    const s = drag;
    if (!s) return;
    cancelAnimationFrame(s.raf);
    if (!s.active) { drag = null; return; }
    justDragged = true; setTimeout(() => { justDragged = false; }, 0);
    if (cancel) s.target = s.idx;
    s.row.classList.add('settling');
    s.row.style.transform = 'translateY(' + ((s.target - s.idx) * s.step) + 'px)';
    if (cancel) s.rows.forEach((r) => { if (r !== s.row) r.style.transform = ''; });
    let done = false;
    const commit = async () => {
      if (done) return;
      done = true;
      const order = keys();
      const k = s.row.dataset.id;
      if (s.target !== s.idx) {
        order.splice(order.indexOf(k), 1);
        order.splice(s.target, 0, k);
      }
      s.rows.forEach((r) => r.classList.add('noanim'));
      if (s.target !== s.idx) onReorder(order);
      s.rows.forEach((r) => { r.style.transform = ''; r.classList.remove('dragging', 'settling', 'shift'); });
      await tick();
      s.rows.forEach((r) => r.classList.remove('noanim'));
      drag = null;
      if (s.target !== s.idx) announce(nameOf(byKey(k)) + ' moved to position ' + (s.target + 1) + ' of ' + s.rows.length);
    };
    s.row.addEventListener('transitionend', (ev) => { if (ev.propertyName === 'transform') commit(); }, { once: true });
    setTimeout(commit, 220);
  }

  // ---- keyboard -------------------------------------------------------------------------------
  // Moves the picked row one step; the keyed list re-renders, so the grip is focused again after.
  async function stepKey(k, dir) {
    const order = keys(), i = order.indexOf(k), j = i + dir;
    if (j < 0 || j >= order.length) return false;
    const tops = new Map(rows().map((r) => [r.dataset.id, r.offsetTop]));
    moving = true;
    order.splice(i, 1); order.splice(j, 0, k);
    onReorder(order);
    await tick();
    const g = list.querySelector('.trow[data-id="' + k + '"] .grip');
    if (g) g.focus();
    moving = false;
    // Glide: put every moved row back where it was, then let it slide to its new slot.
    const moved = rows().filter((r) => tops.get(r.dataset.id) !== r.offsetTop);
    moved.forEach((r) => { r.classList.add('noanim'); r.style.transform = 'translateY(' + (tops.get(r.dataset.id) - r.offsetTop) + 'px)'; });
    void list.offsetHeight;
    moved.forEach((r) => { r.classList.remove('noanim'); r.classList.add('shift'); r.style.transform = ''; setTimeout(() => r.classList.remove('shift'), 180); });
    return true;
  }
  async function drop(ok) {
    if (!kb) return;
    const s = kb; kb = null;
    if (!ok) { onReorder(s.order); await tick(); const g = list.querySelector('.trow[data-id="' + s.key + '"] .grip'); if (g) g.focus(); }
    pickedKey = null;
    const pos = keys().indexOf(s.key) + 1;
    announce(ok ? nameOf(byKey(s.key)) + ' dropped at position ' + pos : 'Move cancelled');
  }
  function onKeydown(e) {
    const g = e.target.closest('.grip');
    if (!g) return;
    const k = g.closest('.trow').dataset.id;
    if (e.key === ' ' || e.key === 'Enter') {
      e.preventDefault();
      if (kb) drop(true);
      else {
        kb = { key: k, from: keys().indexOf(k), order: keys() };
        pickedKey = k;
        announce(nameOf(byKey(k)) + ' picked up, position ' + (kb.from + 1) + ' of ' + items.length + '. Arrow keys to move.');
      }
    } else if (kb && kb.key === k && (e.key === 'ArrowUp' || e.key === 'ArrowDown')) {
      e.preventDefault();
      stepKey(k, e.key === 'ArrowUp' ? -1 : 1).then((ok) => {
        if (!ok) return;
        announce('Position ' + (keys().indexOf(k) + 1) + ' of ' + items.length);
        const el = list.querySelector('.trow[data-id="' + k + '"]');
        if (el) el.scrollIntoView({ block: 'nearest' });
      });
    } else if (kb && e.key === 'Escape') {
      e.preventDefault(); e.stopPropagation(); drop(false);
    }
  }
  function onFocusOut(e) { if (!moving && kb && e.target.closest('.grip')) drop(true); }
  function onClick(e) {
    if (justDragged || e.target.closest('.grip')) return;
    const rowEl = e.target.closest('.trow');
    if (rowEl) onRowClick(byKey(rowEl.dataset.id));
  }
</script>

<!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
<div class="card tlist" role="list" aria-label={labelledby ? undefined : label} aria-labelledby={labelledby}
     bind:this={list}
     onpointerdown={onPointerDown} onpointermove={onPointerMove}
     onpointerup={(e) => { if (drag && e.pointerId === drag.id) finish(false); }}
     onpointercancel={(e) => { if (drag && e.pointerId === drag.id) finish(true); }}
     onlostpointercapture={(e) => { if (drag && drag.active && e.pointerId === drag.id && !drag.row.classList.contains('settling')) finish(false); }}
     onkeydown={onKeydown} onfocusout={onFocusOut} onclick={onClick}>
  {#each items as it (it.key)}
    <div class="trow" class:picked={pickedKey === it.key} role="listitem" data-id={it.key}>
      <button type="button" class="grip" aria-pressed={pickedKey === it.key}
              aria-label={'Reorder ' + nameOf(it) + '. Space to pick up, arrow keys to move, Space to drop.'}>{@html GRIP}</button>
      {@render row?.(it)}
    </div>
  {/each}
</div>

<style>
  .card { border: 1px solid var(--cardb); background: var(--card); border-radius: var(--rc); box-shadow: var(--cshadow); }
  .trow { position: relative; display: grid; grid-template-columns: auto auto 1fr auto; align-items: center; gap: 12px;
          min-height: 62px; padding: 8px 18px 8px 8px; user-select: none; -webkit-user-select: none; cursor: pointer;
          transition: background-color var(--dur-fast) var(--ease); }
  .trow + .trow { border-top: 1px solid var(--rowline); }
  .trow:first-child { border-radius: 9px 9px 0 0; }
  .trow:last-child { border-radius: 0 0 9px 9px; }
  .trow:not(.picked):hover { background: color-mix(in srgb, var(--fg) 4%, var(--card)); }
  .tlist :global(.trow.shift) { transition: transform .15s ease; }
  .tlist :global(.trow.dragging) { z-index: 3; cursor: grabbing; border-radius: 10px; border-top-color: transparent;
    background: color-mix(in srgb, var(--fg) 7%, var(--card));
    box-shadow: 0 0 0 1px var(--line2), 0 8px 20px rgba(0, 0, 0, .26), 0 1px 4px rgba(0, 0, 0, .18);
    transition: box-shadow .15s ease, background-color .15s ease; }
  :global(.wnd[data-theme="light"]) .tlist :global(.trow.dragging) {
    background: #fff; box-shadow: 0 0 0 1px #d4d4d4, 0 10px 26px rgba(0, 0, 0, .16), 0 2px 6px rgba(0, 0, 0, .08); }
  .tlist :global(.trow.dragging.settling) { transition: transform .15s ease, box-shadow .15s ease, background-color .15s ease; }
  .tlist :global(.trow.dragging:hover) { background: color-mix(in srgb, var(--fg) 7%, var(--card)); }
  .trow.picked { background: color-mix(in srgb, var(--fg) 7%, var(--card)); z-index: 2; }
  .tlist :global(.trow.noanim) { transition: none !important; }
  .grip { width: 28px; height: 36px; display: grid; place-items: center; color: var(--ctl); border-radius: 7px;
          cursor: grab; touch-action: none; }
  .grip :global(svg) { width: 20px; height: 20px; fill: currentColor; stroke: none; }
  .trow:hover .grip, .grip:focus-visible { color: var(--fg); }
  .grip:hover { background: var(--hover); color: var(--fg); }
  .tlist :global(.trow.dragging .grip) { cursor: grabbing; color: var(--fg); }
  .trow.picked .grip { color: var(--fill); background: color-mix(in srgb, var(--fill) 14%, transparent); }
</style>
