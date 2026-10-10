<script>
  // One schema row: label and description left, the control for row.type right-aligned to the card
  // edge. Props only, no bridge calls: the page passes values and handlers.
  //   value     the row's own value (ini string or number)
  //   values    the whole map (keybind rows read their sibling keys from it)
  //   onChange  onChange(val) for the row's own key
  //   onSet     onSet(key, val) for a sibling key (the extra-key switches)
  //   live      live(patch) for keybind captures (written immediately by the page)
  //   tab       optional quiet tab name shown beside the label (search results)
  //   extra     { runningModel, onRestart,
  //             profiles: {names, active, onSwitch, onNew, onDelete}, version, onRepo, onAction(name) }
  import Toggle from './Toggle.svelte';
  import Slider from './Slider.svelte';
  import Select from './Select.svelte';
  import Bindings from './Bindings.svelte';
  import PanBinding from './PanBinding.svelte';
  import './bindings.css';
  import AppList from './AppList.svelte';
  import HighRes from './HighRes.svelte';
  import EngineRow from './EngineRow.svelte';
  import ThemePicker from '../prefs/ThemePicker.svelte';
  import ProfilePicker from '../prefs/ProfilePicker.svelte';
  import About from './About.svelte';
  let { row, value = undefined, values = {}, onChange = () => {}, onSet = () => {}, live = () => {}, extra = {}, disabled = false, tab = '' } = $props();

  // Ids so every control is named by its row: label (and the live value for value-bearing ones).
  const rid = $derived('row-' + String(row.key).replace(/[^A-Za-z0-9_-]/g, ''));
  const labelId = $derived(row.label ? rid + '-l' : undefined);
  const descId = $derived(row.desc ? rid + '-d' : undefined);
  const valueId = $derived(rid + '-v');
  const withValue = $derived([labelId, valueId].filter(Boolean).join(' '));
</script>

{#if row.type === 'about'}
  <About version={extra.version} onRepo={extra.onRepo} />
{:else}
  <div class="row" class:disabled class:wide={row.wide} data-key={row.key}>
    <div class="meta">
      {#if row.label}<div class="lrow"><div class="label" id={labelId}>{row.label}</div>{#if row.tag}<span class="tag">{row.tag}</span>{/if}{#if tab}<span class="tab">{tab}</span>{/if}</div>{/if}
      {#if row.desc}<div class="desc" id={descId}>{row.desc}</div>{/if}
    </div>
    <div class="ctl">
      {#if row.type === 'toggle'}
        <Toggle {value} {disabled} onChange={onChange} labelledby={labelId} describedby={descId} />
      {:else if row.type === 'slider'}
        <Slider {value} min={row.min} max={row.max} step={row.step} unit={row.unit} {disabled}
                onChange={onChange} labelledby={labelId} describedby={descId} />
      {:else if row.type === 'select'}
        <Select {value} options={row.options} labels={row.optionLabels} {disabled}
                onChange={onChange} labelledby={labelId} describedby={descId} />
      {:else if row.type === 'engine'}
        <EngineRow {value} options={row.options} labels={row.optionLabels} running={extra.runningModel}
                   onRestart={extra.onRestart} {disabled} onChange={onChange}
                   labelledby={labelId} describedby={descId} />
      {:else if row.type === 'keybind'}
        <!-- Extra keys carry an on/off switch right of the binding; off dims the binding (kept in the ini). -->
        <span class="kwrap" class:off={row.onKey && String(values[row.onKey]) === '0'}>
          {#if row.panArrows}
            <PanBinding {row} {values} {live} {disabled} labelledby={withValue} describedby={descId} {valueId} />
          {:else}
            <Bindings {row} {values} {live} {disabled} labelledby={withValue} describedby={descId} {valueId} />
          {/if}
        </span>
        {#if row.onKey}
          <span class="ksep" aria-hidden="true"></span>
          <Toggle value={values[row.onKey] ?? 1} {disabled} onChange={(v) => onSet(row.onKey, v)}
                  labelledby={labelId} describedby={descId} />
        {/if}
      {:else if row.type === 'applist'}
        <AppList {value} title={row.label} {disabled} onChange={onChange}
                 labelledby={withValue} describedby={descId} {valueId} pick={extra.pick} />
      {:else if row.type === 'highres'}
        <HighRes {value} {disabled} onChange={onChange}
                 labelledby={labelId} describedby={descId} />
      {:else if row.type === 'palette'}
        <ThemePicker value={value ?? 'grey'} onChange={onChange}
                     labelledby={labelId} describedby={descId} />
      {:else if row.type === 'profiles'}
        <ProfilePicker {...(extra.profiles ?? {})} {disabled} labelledby={labelId} describedby={descId} />
      {:else if row.type === 'button'}
        <button type="button" class="chip" {disabled} id={valueId}
                aria-labelledby={withValue} aria-describedby={descId}
                onclick={() => extra.onAction && extra.onAction(row.action)}>{row.btn}</button>
      {/if}
    </div>
  </div>
{/if}

<style>
  .row { display: grid; grid-template-columns: 1fr auto; align-items: center; gap: 24px; min-height: 62px; padding: 8px 18px; }
  .row.disabled { opacity: .45; }
  .row.wide { grid-template-columns: minmax(0, 1fr); gap: 10px; padding-bottom: 12px; }   /* the control sits under the text, full width */
  .row.wide .ctl { justify-content: flex-start; min-width: 0; }
  .meta { min-width: 0; }
  .lrow { display: flex; align-items: baseline; gap: 10px; }
  .tab { font: 11px var(--s); letter-spacing: .02em; color: var(--fg3); opacity: .8; }
  .tag { font: 500 10.5px var(--s); letter-spacing: .03em; color: var(--fg3); padding: 1px 6px;
         border: 1px solid var(--chipb); border-radius: 999px; }
  .label { font: 500 13.5px var(--s); color: var(--fg); }
  .desc { margin-top: 1px; font: 12.5px var(--s); color: var(--fg3); }
  .ctl { display: flex; align-items: center; justify-content: flex-end; gap: 6px; }
  .chip { height: 28px; padding: 0 12px; border-radius: var(--srad); background: var(--chip);
          border: 1px solid var(--chipb); color: var(--fg); font: 13px var(--s); }
  .chip:hover:not(:disabled) { border-color: var(--outline); }
</style>
