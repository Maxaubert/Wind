<script>
  // One schema row: label and description left, the control for row.type right-aligned to the card
  // edge. Props only, no bridge calls: the page passes values and handlers.
  //   value     the row's own value (ini string or number)
  //   values    the whole map (keybind rows read their sibling keys from it)
  //   onChange  onChange(val) for the row's own key
  //   live      live(patch) for keybind captures (written immediately by the page)
  //   extra     { mpoNeedsRestart, runningModel, onRestart, theme, onTheme, profiles: {names, active,
  //             onSwitch, onCreate, onRename, onDuplicate, onDelete}, version, onRepo, onAction(name) }
  import Toggle from './Toggle.svelte';
  import Slider from './Slider.svelte';
  import Select from './Select.svelte';
  import Keycaps from './Keycaps.svelte';
  import AppList from './AppList.svelte';
  import HighRes from './HighRes.svelte';
  import EngineRow from './EngineRow.svelte';
  import ThemeRow from './ThemeRow.svelte';
  import Profiles from '../general/Profiles.svelte';
  import About from './About.svelte';
  let { row, value = undefined, values = {}, onChange = () => {}, live = () => {}, extra = {}, disabled = false } = $props();

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
  <div class="row" class:disabled data-key={row.key}>
    <div class="meta">
      {#if row.label}<div class="label" id={labelId}>{row.label}</div>{/if}
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
        <Keycaps {row} {values} {live} {disabled} labelledby={withValue} describedby={descId} {valueId} />
      {:else if row.type === 'applist'}
        <AppList {value} title={row.label} {disabled} onChange={onChange}
                 labelledby={withValue} describedby={descId} {valueId} pick={extra.pick} />
      {:else if row.type === 'highres'}
        <HighRes {value} {disabled} onChange={onChange} needsRestart={!!extra.mpoNeedsRestart}
                 labelledby={labelId} describedby={descId} tagId={rid + '-t'} />
      {:else if row.type === 'theme'}
        <ThemeRow value={extra.theme ?? value ?? 'auto'} onChange={extra.onTheme ?? onChange}
                  labelledby={labelId} describedby={descId} />
      {:else if row.type === 'profiles'}
        <Profiles {...(extra.profiles ?? {})} {disabled} labelledby={labelId} describedby={descId} />
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
  .meta { min-width: 0; }
  .label { font: 500 13.5px var(--s); color: var(--fg); }
  .desc { margin-top: 1px; font: 12.5px var(--s); color: var(--fg3); }
  .ctl { display: flex; align-items: center; justify-content: flex-end; gap: 6px; }
  .chip { height: 28px; padding: 0 12px; border-radius: var(--srad); background: var(--chip);
          border: 1px solid var(--chipb); color: var(--fg); font: 13px var(--s); }
  .chip:hover:not(:disabled) { border-color: var(--outline); }
</style>
