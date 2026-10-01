<script>
  // On/off switch. `value` is the ini number (1 = on). A real checkbox with role=switch sits over
  // the drawn track, so keyboard and screen readers get a native control.
  let { value = 0, onChange = () => {}, disabled = false, labelledby, describedby } = $props();
</script>

<label class="tg" class:disabled>
  <input type="checkbox" role="switch" {disabled} checked={Number(value) === 1}
         aria-labelledby={labelledby} aria-describedby={describedby}
         onchange={(e) => onChange(e.currentTarget.checked ? 1 : 0)} />
  <span class="track" aria-hidden="true"><span class="knob"></span></span>
</label>

<style>
  .tg { position: relative; display: inline-block; width: 36px; height: 20px; flex: none; }
  .tg.disabled { opacity: .45; pointer-events: none; }
  input { position: absolute; inset: 0; width: 100%; height: 100%; margin: 0; opacity: 0; cursor: pointer;
          -webkit-appearance: none; appearance: none; }
  .track { position: absolute; inset: 0; border-radius: 10px; background: var(--track);
           border: 1px solid var(--chipb); transition: background .15s, border-color .15s; }
  .knob { position: absolute; top: 3px; left: 3px; width: 12px; height: 12px; border-radius: 50%;
          background: var(--fg3); transition: transform .15s, background .15s; }
  input:checked + .track { background: var(--fill); border-color: var(--fill); }
  input:checked + .track .knob { transform: translateX(16px); background: #fff; }
  input:focus-visible + .track { outline: 2px solid var(--fg); outline-offset: 2px; }
</style>
