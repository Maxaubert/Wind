/**
 * Builds the video loop setup plays, from a source clip.
 *
 *     node installer/make-loop.mjs "C:\path\to\clip.mp4" [--start 0] [--len 16] [--fps 24]
 *
 * Out: installer/media/800/v/000.jpg ...
 *
 * Two things worth knowing:
 *
 *  - The clip does not loop, so we make it loop. The last K frames are crossfaded onto the
 *    first K, which leaves the last frame running into frame 0 with a smaller step than an
 *    ordinary frame-to-frame one.
 *
 *  - The install screen cannot animate: NSIS runs the section on the script thread, so
 *    nothing can call back into script while files are being written. That screen draws one
 *    frame and lets the progress bar carry the motion.
 *
 * Needs ffmpeg and ImageMagick (`magick`) on PATH.
 */
import { execFileSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const MEDIA = path.join(HERE, 'media')

const argv = process.argv.slice(2)
const SRC = argv[0]
const opt = (name, dflt) => {
  const i = argv.indexOf(`--${name}`)
  return i >= 0 ? Number(argv[i + 1]) : dflt
}
const START = opt('start', 0)
const LEN = opt('len', 16)
const FPS = opt('fps', 24)
const K = opt('fade', 36) // crossfaded frames, a second and a half at 24 fps
// One size for every display. The overlay stays per DPI so type is always sharp, but the
// footage is defocused motion: an 800 to 1440 upscale is invisible on it, and halving the
// payload is what buys a long loop at a sane download size.
const SIZES = [{ w: 800, h: 600, q: 62 }]

const run = (bin, args) => execFileSync(bin, args, { stdio: ['ignore', 'pipe', 'pipe'] })

if (!SRC || !fs.existsSync(SRC)) {
  console.error('usage: node installer/make-loop.mjs <clip> [--start s] [--len s] [--fps n] [--fade n]')
  process.exit(1)
}

for (const { w, h, q } of SIZES) {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'wind-loop-'))
  const raw = path.join(tmp, 'raw')
  fs.mkdirSync(raw)

  run('ffmpeg', ['-y', '-v', 'error', '-ss', String(START), '-t', String(LEN), '-i', SRC,
    '-vf', [`fps=${FPS}`, `scale=${w}:${h}:force_original_aspect_ratio=increase`, `crop=${w}:${h}`].join(','),
    path.join(raw, '%03d.png')])

  // However many frames the source really yielded: asking for one more than exists is the
  // difference between a loop and a crash.
  const have = fs.readdirSync(raw).length
  const n = have - K
  if (n < K * 2) throw new Error(`only ${have} frames available, need at least ${K * 3}`)

  const f = (i) => path.join(raw, `${String(i + 1).padStart(3, '0')}.png`)
  const vdir = path.join(MEDIA, String(w), 'v')
  fs.rmSync(vdir, { recursive: true, force: true })
  fs.mkdirSync(vdir, { recursive: true })

  for (let i = 0; i < n; i++) {
    const dst = path.join(vdir, `${String(i).padStart(3, '0')}.jpg`)
    if (i < K) {
      // out[i] = frame[n+i] fading out under frame[i] fading in, so the wrap from the last
      // frame back to the first is already in progress by the time it happens
      const pct = Math.round((100 * i) / K)
      const blend = path.join(tmp, 'b.png')
      run('magick', [f(n + i), f(i), '-define', `compose:args=${pct}`, '-compose', 'blend', '-composite', blend])
      run('magick', [blend, '-quality', String(q), '-sampling-factor', '4:2:0', '-strip', dst])
    } else {
      run('magick', [f(i), '-quality', String(q), '-sampling-factor', '4:2:0', '-strip', dst])
    }
  }

  fs.rmSync(tmp, { recursive: true, force: true })
  const bytes = fs.readdirSync(vdir).reduce((a, x) => a + fs.statSync(path.join(vdir, x)).size, 0)
  console.log(`${w}x${h}: ${n} frames of ${have} available, ${(bytes / 1e6).toFixed(1)} MB`)
  console.log(`   set FRAMES to ${n} and TICK to ${Math.round(1000 / FPS)} in installer/video.nsh`)
}
