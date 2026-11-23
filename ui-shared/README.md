# ui-shared

Settings UI files that the Windows app (`ui/`, WebView2) and Wind for Mac (`mac/ui/`, WKWebView) use
unchanged: design files, themes, generic controls and shell pieces. Each app imports them as
`$shared/<path>` (an alias in its `vite.config.js`); files in here import each other relatively.

- Small platform differences are props, set by each app: `Sidebar` (`searchHint`, `trafficLights` for
  the Mac's traffic-light corner), `About` (`onRepo` for the Windows star button, `note`) and `Results`
  (`Row`, the app's own `SettingRow`).
- `tray/`: the quick-control list page (Windows Tray menu, Mac Menu bar): `listModel.js` (pure parse and
  write rules, given each app's items and five config key names), `ItemListsPage.svelte` (props `model`,
  `where`) and `DragList.svelte`. Windows passes `ui/src/tray/trayModel.js`, the Mac
  `mac/ui/src/menubar/menuModel.js`.

- Only platform-neutral files belong here. Anything that reads the schema, the bridge or a platform
  setting stays in its app.
- `tools/gen-themes.cjs` writes `src/design/themes.css` and `themes.js`. Edit the generator, not
  the output.
- A change here ships in both apps: run `npx playwright test` in `ui/` and in `mac/ui/` (WebKit and
  Chromium), and both CI workflows (`build.yml`, `mac.yml`) run on it.
