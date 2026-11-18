# Wind for Mac settings UI

A copy of Windows Wind's settings UI (`Maxaubert/Wind`, `ui/` at commit `982e162`), adapted for the Mac:
Mac settings schema and keys, a WKWebView bridge (`src/bridge.js`), Mac keybind names and rules, the
macOS card and the Mac first-run setup. The two copies diverge on purpose; port Windows design or
component fixes across by hand, comparing against that commit.

Files that are the same in both apps live once in `../../ui-shared/src` and are imported as `$shared/...`
(Sidebar, Results, Prompt, About, Slider, Toggle, the themes and more). A change there ships on both, so
run the Windows `ui/` suite too.

- `npm ci` then `npm run build` writes `dist/`, which `scripts/build.sh` bundles into
  `Wind.app/Contents/Resources/ui`.
- `npx playwright test` runs the UI tests against the mock bridge; `npm run test:rules` the Node tests.
