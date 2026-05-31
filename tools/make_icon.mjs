// Generate assets/wind.ico from assets/wind-badge.svg.
//
// Renders the SVG at each icon size with Playwright's bundled Chromium (faithful
// stroke/arc rendering, transparent corners outside the rounded badge), then packs
// the PNGs into a single multi-resolution .ico with ImageMagick (`magick`).
//
// Run from the repo root:  node tools/make_icon.mjs
// Requirements (dev-only; the committed wind.ico means the normal build needs neither):
//   - Playwright installed under ui/node_modules (it is, for the UI e2e tests)
//   - ImageMagick 7 on PATH (the `magick` command)
import { createRequire } from 'module';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, resolve } from 'path';
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from 'fs';
import { tmpdir } from 'os';
import { spawnSync } from 'child_process';

const __dirname = dirname(fileURLToPath(import.meta.url));
const root = resolve(__dirname, '..');
const require = createRequire(import.meta.url);
// Resolve Playwright from the UI package (no top-level dependency on it).
const { chromium } = require(resolve(root, 'ui', 'node_modules', 'playwright'));

const SIZES = [256, 128, 64, 48, 40, 32, 24, 16];
const svg = readFileSync(resolve(root, 'assets', 'wind-badge.svg'), 'utf8');
const tmp = mkdtempSync(resolve(tmpdir(), 'windicon-'));

const browser = await chromium.launch();
const page = await browser.newPage();
const pngs = [];
for (const s of SIZES) {
  // Page is transparent; the SVG fills exactly s x s so the screenshot is the icon.
  const html = `<!doctype html><html><head><style>
    html,body{margin:0;padding:0;background:transparent}
    svg{display:block;width:${s}px;height:${s}px}
  </style></head><body>${svg}</body></html>`;
  await page.setViewportSize({ width: s, height: s });
  await page.setContent(html, { waitUntil: 'networkidle' });
  const out = resolve(tmp, `icon-${s}.png`);
  await page.locator('svg').screenshot({ path: out, omitBackground: true });
  pngs.push(out);
}
await browser.close();

const ico = resolve(root, 'assets', 'wind.ico');
const r = spawnSync('magick', [...pngs, ico], { stdio: 'inherit', shell: true });
rmSync(tmp, { recursive: true, force: true });
if (r.status !== 0) { console.error('magick failed (is ImageMagick 7 on PATH?)'); process.exit(1); }
console.log('Wrote', ico);
