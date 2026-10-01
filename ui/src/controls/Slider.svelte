<script>
  // Range input with a teal fill left of the thumb and a bright end-cap knob (3x9px, no shadow).
  // The value is right-aligned in tabular numbers. `unit` is spoken, not drawn.
  let { value = 0, min = 0, max = 100, step = 1, unit = '', onChange = () => {}, disabled = false,
        labelledby, describedby } = $props();
  const pct = $derived(max > min ? Math.min(100, Math.max(0, ((Number(value) - min) / (max - min)) * 100)) : 0);
  const text = $derived(unit ? `${value} ${unit}` : undefined);
</script>

<div class="sl" class:disabled>
  <input type="range" {min} {max} {step} {value} {disabled} style="--pct:{pct}%"
         aria-labelledby={labelledby} aria-describedby={describedby} aria-valuetext={text}
         oninput={(e) => onChange(e.currentTarget.value)} />
  <span class="val" aria-hidden="true">{value}</span>
</div>

<style>
  .sl { display: inline-flex; align-items: center; gap: 12px; }
  .sl.disabled { opacity: .45; }
  input { -webkit-appearance: none; appearance: none; width: 180px; height: 20px; margin: 0;
          background: transparent; cursor: pointer; }
  input::-webkit-slider-runnable-track {
    height: 4px; border-radius: 2px;
    background: linear-gradient(to right, var(--fill) 0 var(--pct), var(--track) var(--pct) 100%); }
  input::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 3px; height: 9px;
    margin-top: -2.5px; border-radius: 1px; background: var(--thumb); border: 0; box-shadow: none; }
  input:focus-visible { outline: 2px solid var(--fg); outline-offset: 3px; border-radius: 2px; }
  .val { min-width: 4ch; text-align: right; font-variant-numeric: tabular-nums; color: var(--fg2); }
</style>
