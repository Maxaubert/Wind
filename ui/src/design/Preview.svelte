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
  let active = $state('hotkeys');
  let count = $state(Number(q.get('count') ?? 2));
  const calls = (window.__calls = []);
  const log = (name, arg) => calls.push(arg === undefined ? name : [name, arg]);

  const groups = [
    { id: 'hotkeys', label: 'Hotkeys', icon: 'hotkeys' },
    { id: 'zoom', label: 'Zoom', icon: 'zoom' },
    { id: 'view', label: 'View', icon: 'view' },
    { id: 'screen', label: 'Screen', icon: 'screen' },
  ];
  const bottom = [
    { id: 'prefs', label: 'Preferences', icon: 'general' },
    { id: 'tray', label: 'Tray menu', icon: 'tray' },
    { id: 'about', label: 'About', icon: 'about' },
  ];
</script>

<div class="wnd app" data-theme={theme}>
  <TitleBar onMinimize={() => log('minimize')} onMaximize={() => log('maximize')} onClose={() => log('close')} />
  <div class="body">
    <Sidebar {groups} {bottom} {active} version="0.15.4"
             onSelect={(id) => { active = id; log('select', id); }} onSearch={(v) => log('search', v)} />
    <main class="main">
      <Banner title="Zoom" description="How far and how fast to zoom." icon="zoom" />
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
