<script>
  // Range input with a teal fill left of the thumb and a bright end-cap knob (3x9px, no shadow).
  // The value is right-aligned in tabular numbers. `unit` is spoken, not drawn.
  let { value = 0, min = 0, max = 100, step = 1, unit = '', onChange = () => {}, disabled = false,
        labelledby, describedby } = $props();
  const pct = $derived(max > min ? Math.min(100, Math.max(0, ((Number(value) - min) / (max - min)) * 100)) : 0);
  const text = $derived(unit ? `${value} ${unit}` : undefined);
  // Drawn readout: decimals follow the step, the unit is a short suffix ('times' -> 12x, 1.25x).
  const SUFFIX = { times: 'x', '%': '%', ms: ' ms', seconds: ' s' };
  const decimals = $derived((String(step).split('.')[1] || '').length);
  const shown = $derived(Number(value).toFixed(decimals) + (SUFFIX[unit] ?? ''));

  // A drag fires input on every pixel and each onChange is an ini write through the host. Send the
  // first at once, then at most one per 50 ms (the latest value, trailing); release flushes.
  const GAP = 50;
  let last = 0, timer = null, pending = null;
  function send(v) { last = performance.now(); pending = null; onChange(v); }
  function flush() {
    if (timer) { clearTimeout(timer); timer = null; }
    if (pending !== null) send(pending);
  }
  function onInput(v) {
    pending = v;
    if (timer) return;
    const wait = GAP - (performance.now() - last);
    if (wait <= 0) send(v);
    else timer = setTimeout(() => { timer = null; if (pending !== null) send(pending); }, wait);
  }
  $effect(() => () => flush());
</script>

<div class="sl" class:disabled>
  <input type="range" {min} {max} {step} {value} {disabled} style="--pct:{pct}%"
         aria-labelledby={labelledby} aria-describedby={describedby} aria-valuetext={text}
         oninput={(e) => onInput(e.currentTarget.value)} onchange={flush} onblur={flush} />
  <span class="val" aria-hidden="true">{shown}</span>
</div>

<style>
  .sl { display: inline-flex; max-width: 100%; min-width: 0; align-items: center; gap: 6px; }
  .sl.disabled { opacity: .45; }
  input { -webkit-appearance: none; appearance: none; width: 220px; max-width: 100%; flex: 1 1 120px; min-width: 0; height: 20px; margin: 0 10px 0 0;
          background: transparent; cursor: pointer; }
  input::-webkit-slider-runnable-track {
    height: 3px; border-radius: 2px;
    background: linear-gradient(to right, var(--fill) 0 var(--pct), var(--track) var(--pct) 100%); }
  input::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 3px; height: 9px;
    margin-top: -3px; border-radius: 1px; background: var(--thumb); border: 0; box-shadow: none; }
  input:focus-visible { outline: 2px solid var(--fg); outline-offset: 3px; border-radius: 2px; }
  .val { width: 46px; margin-left: 4px; text-align: right; font: 500 13px var(--s); font-variant-numeric: tabular-nums; color: var(--fg); }
</style>
