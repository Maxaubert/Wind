<script>
  // Dev/test harness: every shell component rendered with props only, no bridge. Served by Vite
  // dev at /preview.html (not part of the production build). ?theme=light switches the palette.
  import './tokens.css';
  import TitleBar from '../shell/TitleBar.svelte';
  import Sidebar from '../shell/Sidebar.svelte';
  import Banner from '../shell/Banner.svelte';
  import Card from '../shell/Card.svelte';
  import SaveCapsule from '../shell/SaveCapsule.svelte';

  const q = new URLSearchParams(location.search);
  let theme = $state(q.get('theme') === 'light' ? 'light' : 'dark');
  let mode = $state(theme);
  let active = $state('zoom');
  let count = $state(Number(q.get('count') ?? 2));
  const calls = (window.__calls = []);
  const log = (name, arg) => calls.push(arg === undefined ? name : [name, arg]);

  const groups = [
    { id: 'zoom', label: 'Zoom', icon: 'zoom' },
    { id: 'move', label: 'Moving around', icon: 'move' },
    { id: 'cursor', label: 'Cursor', icon: 'cursor' },
    { id: 'typing', label: 'Follow typing', icon: 'typing' },
    { id: 'colour', label: 'Colour', icon: 'colour' },
    { id: 'general', label: 'General', icon: 'general' },
  ];
  const expert = [
    { id: 'adv', label: 'Advanced', icon: 'adv' },
    { id: 'about', label: 'About', icon: 'about' },
  ];
</script>

<div class="wnd app" data-theme={theme}>
  <TitleBar themeMode={mode} onTheme={(m) => { mode = m; log('theme', m); }}
            onMinimize={() => log('minimize')} onClose={() => log('close')} />
  <div class="body">
    <Sidebar {groups} {expert} {active} version="0.15.4"
             onSelect={(id) => { active = id; log('select', id); }} onSearch={(v) => log('search', v)} />
    <main class="main">
      <Banner title="Zoom" description="Keys, limits and speed for magnifying the screen." icon="zoom" />
      <Card caption="Keys">
        <div class="row">Zoom in</div>
        <div class="row">Zoom out</div>
      </Card>
    </main>
  </div>
  <SaveCapsule {count} onSave={() => log('save')} onDiscard={() => log('discard')} />
</div>

<style>
  .app { width: 1120px; height: 760px; display: grid; grid-template-rows: 38px 1fr; position: relative; overflow: hidden; }
  .body { display: grid; grid-template-columns: 240px 1fr; min-height: 0; }
  .main { position: relative; min-height: 0; overflow: hidden; padding: 0 40px; }
  .row { padding: 20px 18px; }
</style>
