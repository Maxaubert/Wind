/**
 * Renders installer/over.html into the alpha overlays the installer composites over its
 * video loop, plus the rectangles NSIS needs to know about.
 *
 *     node installer/make-over.mjs 1440
 *     node installer/make-over.mjs 960
 *
 * Alpha is not captured, it is solved for. Each screen is rendered twice, once on black and
 * once on white; for a pixel of colour C at coverage a those give A = C*a and
 * B = C*a + (1-a), so a = 1 - (B - A) and C = A / a. That is exact for antialiased type,
 * soft shadows and the glow under a button, none of which a screenshot of a transparent
 * window reliably brings back on Windows.
 *
 * The result is a STRAIGHT (non-premultiplied) alpha PNG, which is what video.nsh's
 * GdipDrawImageRectI expects.
 *
 * Out: installer/media/<size>/o/<screen>[-hot-<control>].png
 *      installer/media/<size>/o/box-{on,off}.png
 *      installer/over.nsh   rectangles, in 640x480 units, written by the 1440 pass only
 *
 * Needs ImageMagick (`magick`) on PATH.
 */
import { execFileSync } from 'node:child_process'
import { createRequire } from 'node:module'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const OUT = path.join(HERE, 'media')

// Playwright is a devDependency of ui/, not of the repo root, and there is no root
// node_modules to resolve through. Resolving from ui/package.json keeps the installer from
// adding a second copy of a 100 MB browser automation library just to take screenshots.
const require = createRequire(path.join(HERE, '..', 'ui', 'package.json'))
const { chromium } = require('playwright')

const WIDTH = Number(process.argv[2]) || 1440
const SCALE = WIDTH / 640
const DIR = String(WIDTH)
const SCREENS = ['welcome', 'setup', 'copy', 'done']

// which control is drawn hot on which screen, and so which crops we need twice
const HOT = {
  welcome: ['next', 'close', 'min'],
  setup: ['next', 'back', 'close', 'min'],
  copy: ['close', 'min'],
  done: ['next', 'close', 'min']
}

const magick = (args) => execFileSync('magick', args, { stdio: ['ignore', 'pipe', 'pipe'] })

/** A = over black, B = over white -> straight RGBA */
function solveAlpha(onBlack, onWhite, out, tmp) {
  const alpha = path.join(tmp, 'a.png')
  const colour = path.join(tmp, 'c.png')
  // a = 1 - (B - A). ImageMagick's Minus is second-minus-first, so the black pass goes
  // first here even though it is the one being subtracted.
  magick([onBlack, onWhite, '-compose', 'Minus', '-composite', '-colorspace', 'gray', '-negate', alpha])
  // C = A / a, and where a is 0 the colour is arbitrary, so let it be black
  magick([onBlack, alpha, '-compose', 'divide', '-composite', colour])
  magick([colour, alpha, '-alpha', 'off', '-compose', 'copy_opacity', '-composite', out])
  fs.rmSync(alpha, { force: true })
  fs.rmSync(colour, { force: true })
}

const W = Math.round(640 * SCALE)
const H = Math.round(480 * SCALE)

// --disable-lcd-text forces grayscale antialiasing. With subpixel (LCD) antialiasing the
// three channels get different coverage, the solve produces a different alpha per channel,
// and type comes back with coloured fringes on any frame that is not the one it was
// rendered against.
const browser = await chromium.launch({ args: ['--disable-lcd-text', '--force-color-profile=srgb'] })
// deviceScaleFactor rather than a CSS zoom: the page stays authored in 640x480 units, so
// rects() needs no conversion, while the capture comes out at the display's real pixels.
const page = await browser.newPage({
  viewport: { width: 1280, height: 480 },
  deviceScaleFactor: SCALE
})
await page.goto('file://' + path.join(HERE, 'over.html').replace(/\\/g, '/'))
await page.waitForTimeout(250)

const odir = path.join(OUT, DIR, 'o')
fs.rmSync(odir, { recursive: true, force: true })
fs.mkdirSync(odir, { recursive: true })
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'wind-over-'))

async function shootSolved(file) {
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))))
  const shot = path.join(tmp, 'shot.png')
  await page.screenshot({ path: shot, clip: { x: 0, y: 0, width: 1280, height: 480 } })
  const a = path.join(tmp, 'k.png')
  const b = path.join(tmp, 'w.png')
  magick([shot, '-crop', `${W}x${H}+0+0`, '+repage', a])
  magick([shot, '-crop', `${W}x${H}+${W}+0`, '+repage', b])
  solveAlpha(a, b, file, tmp)
}

async function capture(s, hot, boxes, file) {
  await page.evaluate(([a, b, c]) => window.render(a, b, c), [s, hot, boxes])
  await shootSolved(file)
}

// The shade, the rim and the caption scrim are the same on every screen, so they are one
// overlay drawn under the type rather than eighteen copies of the same gradient.
await page.evaluate(() => window.renderBack())
await shootSolved(path.join(odir, 'back.png'))
console.log('  back.png')

const rects = {}

for (let s = 0; s < SCREENS.length; s++) {
  const name = SCREENS[s]
  // Every screen is rendered with its checkboxes left out: setup paints those itself, so
  // the art does not need a variant per combination of states.
  for (const hot of [null, ...HOT[name]]) {
    const out = `${name}${hot ? `-hot-${hot}` : ''}.png`
    await capture(s, hot, 'none', path.join(odir, out))
    console.log(`  ${out}`)
  }

  // rectangles are per screen, because the same button sits somewhere else on each of
  // them: O_WELCOME_NEXT is not O_DONE_NEXT
  await page.evaluate(([a]) => window.render(a, null, 'none'), [s])
  const r = await page.evaluate(() => window.rects())
  for (const [k, v] of Object.entries(r)) rects[`${name.toUpperCase()}_${k}`] = v
}

// The two states of one box, cut out to be stamped at runtime. Both screens that carry
// options use the same 18x18 box, so one pair serves all three of them.
{
  await page.evaluate(() => window.render(3, null, 'none'))
  const box = (await page.evaluate(() => window.rects())).BOX_RUN
  for (const state of ['on', 'off']) {
    const solved = path.join(tmp, `box-${state}-solved.png`)
    await capture(3, null, state, solved)
    magick([solved, '-crop',
      `${Math.round(box.w * SCALE)}x${Math.round(box.h * SCALE)}+${Math.round(box.x * SCALE)}+${Math.round(box.y * SCALE)}`,
      '+repage', path.join(odir, `box-${state}.png`)])
    console.log(`  box-${state}.png`)
  }
}

// One source of truth for the rectangles: both overlay sets are the same layout in the same
// 640x480 units, so only one pass writes the file.
if (WIDTH === 1440) {
  const lines = [
    '; Generated by make-over.mjs from over.html. Do not edit: run',
    ';   node installer/make-over.mjs 1440',
    ';   node installer/make-over.mjs 960',
    '; Rectangles are in 640x480 units; the installer scales them to its window.',
    ''
  ]
  for (const [k, b] of Object.entries(rects)) {
    lines.push(`!define O_${k}_X ${b.x}`, `!define O_${k}_Y ${b.y}`,
               `!define O_${k}_W ${b.w}`, `!define O_${k}_H ${b.h}`)
  }
  fs.writeFileSync(path.join(HERE, 'over.nsh'), lines.join('\n') + '\n')
}

fs.rmSync(tmp, { recursive: true, force: true })
await browser.close()

const files = fs.readdirSync(odir)
const bytes = files.reduce((n, x) => n + fs.statSync(path.join(odir, x)).size, 0)
console.log(`overlays @${WIDTH}: ${files.length} files, ${Object.keys(rects).length} rectangles, ${(bytes / 1e6).toFixed(1)} MB`)
