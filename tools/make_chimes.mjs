// Synthesize the Wind tray enable / disable chimes (issue #315).
//
// Pure Node, no network, no dependencies, fully deterministic (seeded noise): the same script always
// writes byte-identical WAVs. Output is 48 kHz, 16-bit, mono.
//
//   node tools/make_chimes.mjs           write assets/sounds/*.wav, candidates/*.wav and the preview page
//   node tools/make_chimes.mjs --check   only verify the committed WAVs (exit 1 on a problem)
//
// The default pair is chime_on.wav (rising) and chime_off.wav (the same two notes falling, a little
// softer). Three alternative timbres land in assets/sounds/candidates/ with a preview page.
import { writeFileSync, readFileSync, readdirSync, existsSync, mkdirSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, resolve } from 'path';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const SOUNDS = resolve(root, 'assets', 'sounds');
const CAND = resolve(SOUNDS, 'candidates');
const RATE = 48000;
const TOTAL_S = 0.46;                 // every chime is this long
const ATTACK_S = 0.004;               // 4 ms raised-cosine attack, so there is no click
const FADE_S = 0.06;                  // raised-cosine fade-out over the last 60 ms
const dbfs = (db) => Math.pow(10, db / 20);

// A voice is a set of partials, each [frequency ratio, level, exponential decay time constant in s].
// Fast-decaying upper partials give the soft "struck" bell / marimba shape.
const VOICES = {
  bell:    { partials: [[1, 1.0, 0.13], [2, 0.22, 0.06], [3, 0.07, 0.03]] },
  glassy:  { partials: [[1, 1.0, 0.15], [2.76, 0.18, 0.07], [5.4, 0.06, 0.035]] },
  marimba: { partials: [[1, 1.0, 0.085], [3.93, 0.20, 0.03], [9.2, 0.05, 0.012]] },
  pluck:   { partials: [[1, 1.0, 0.10], [2, 0.12, 0.05]], breath: 0.10 },
};

// Sets: voice, the two notes (Hz, low then high), the start of the second note, the echo taps [delay s, gain].
const SETS = {
  default: { voice: 'bell',    notes: [659.25, 880.00],   gap: 0.12, taps: [[0.085, 0.24], [0.17, 0.11]] },
  glassy:  { voice: 'glassy',  notes: [1046.50, 1396.91], gap: 0.11, taps: [[0.09, 0.22], [0.18, 0.10]] },
  marimba: { voice: 'marimba', notes: [523.25, 783.99],   gap: 0.13, taps: [[0.08, 0.20], [0.16, 0.08]] },
  pluck:   { voice: 'pluck',   notes: [783.99, 1046.50],  gap: 0.12, taps: [[0.10, 0.26], [0.20, 0.12]] },
};

function rng(seed) {                  // small deterministic LCG for the breath noise
  let s = seed >>> 0;
  return () => ((s = (Math.imul(s, 1664525) + 1013904223) >>> 0) / 4294967296) * 2 - 1;
}

function renderNote(buf, voice, freq, start, level) {
  const i0 = Math.round(start * RATE);
  const atk = Math.round(ATTACK_S * RATE);
  const rand = rng(Math.round(freq * 100));
  let lp = 0;
  for (let i = i0; i < buf.length; i++) {
    const t = (i - i0) / RATE;
    let v = 0;
    for (const [ratio, amp, tau] of voice.partials) {
      v += amp * Math.exp(-t / tau) * Math.sin(2 * Math.PI * freq * ratio * t);
    }
    if (voice.breath) {               // a short low-passed noise puff on the attack: the "air"
      lp += 0.25 * (rand() - lp);
      v += voice.breath * lp * Math.exp(-t / 0.018);
    }
    const a = i - i0 < atk ? 0.5 - 0.5 * Math.cos(Math.PI * (i - i0) / atk) : 1;
    buf[i] += level * a * v;
  }
}

function synth(set, notes, peakDb) {
  const voice = VOICES[set.voice];
  const n = Math.round(TOTAL_S * RATE);
  const dry = new Float64Array(n);
  renderNote(dry, voice, notes[0], 0, 1.0);
  renderNote(dry, voice, notes[1], set.gap, 0.9);
  // Light echo tail: two quiet delayed copies.
  const out = Float64Array.from(dry);
  for (const [d, g] of set.taps) {
    const off = Math.round(d * RATE);
    for (let i = off; i < n; i++) out[i] += g * dry[i - off];
  }
  // Remove any DC, fade out, then normalise the peak.
  let mean = 0; for (const x of out) mean += x; mean /= n;
  const fade = Math.round(FADE_S * RATE);
  let peak = 0;
  for (let i = 0; i < n; i++) {
    out[i] -= mean;
    const k = n - 1 - i;
    if (k < fade) out[i] *= 0.5 - 0.5 * Math.cos(Math.PI * k / fade);
    peak = Math.max(peak, Math.abs(out[i]));
  }
  const g = dbfs(peakDb) / peak;
  const pcm = new Int16Array(n);
  for (let i = 0; i < n; i++) pcm[i] = Math.max(-32767, Math.min(32767, Math.round(out[i] * g * 32767)));
  pcm[0] = 0; pcm[n - 1] = 0;
  return pcm;
}

function wav(pcm) {
  const data = Buffer.alloc(pcm.length * 2);
  pcm.forEach((s, i) => data.writeInt16LE(s, i * 2));
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8);
  h.write('fmt ', 12); h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(1, 22);
  h.writeUInt32LE(RATE, 24); h.writeUInt32LE(RATE * 2, 28); h.writeUInt16LE(2, 32); h.writeUInt16LE(16, 34);
  h.write('data', 36); h.writeUInt32LE(data.length, 40);
  return Buffer.concat([h, data]);
}

// Verification shared by generation and --check. Same rules as src/chime_wav.h (the C++ doctest).
export function verifyWav(file) {
  const b = readFileSync(file);
  const errs = [];
  if (b.toString('ascii', 0, 4) !== 'RIFF' || b.toString('ascii', 8, 12) !== 'WAVE') return ['not a WAV'];
  if (b.readUInt16LE(20) !== 1 || b.readUInt16LE(22) !== 1 || b.readUInt32LE(24) !== RATE || b.readUInt16LE(34) !== 16)
    errs.push('format is not 48 kHz 16-bit mono PCM');
  const len = b.readUInt32LE(40) / 2;
  const s = (i) => b.readInt16LE(44 + i * 2);
  let peak = 0, sum = 0;
  for (let i = 0; i < len; i++) { peak = Math.max(peak, Math.abs(s(i))); sum += s(i); }
  const dur = len / RATE;
  if (dur < 0.35 || dur > 0.6) errs.push(`duration ${dur.toFixed(3)} s outside 0.35-0.6`);
  if (peak >= 32767) errs.push('clips');
  if (peak > 32767 * dbfs(-5)) errs.push('peak above -5 dBFS');
  if (peak < 32767 * dbfs(-12)) errs.push('peak below -12 dBFS');
  if (Math.abs(sum / len) > 32767 * 0.002) errs.push('DC offset');
  if (Math.abs(s(0)) > 64 || Math.abs(s(len - 1)) > 64) errs.push('click at the start or end');
  let tail = 0; for (let i = len - 48; i < len; i++) tail = Math.max(tail, Math.abs(s(i)));
  if (tail > 32767 * 0.01) errs.push('tail not faded');
  return errs;
}

function allWavs() {
  const out = [];
  for (const d of [SOUNDS, CAND]) {
    if (existsSync(d)) for (const f of readdirSync(d)) if (f.endsWith('.wav')) out.push(resolve(d, f));
  }
  return out;
}

function check() {
  const files = allWavs();
  let bad = 0;
  for (const f of files) {
    const e = verifyWav(f);
    console.log(`${e.length ? 'FAIL' : 'ok  '} ${f.slice(root.length + 1)}${e.length ? ': ' + e.join('; ') : ''}`);
    bad += e.length ? 1 : 0;
  }
  if (!files.length) { console.log('no WAVs found'); bad = 1; }
  return bad === 0;
}

function page() {
  const rows = [
    ['Default: warm bell', '../chime_on.wav', '../chime_off.wav'],
    ['Glassy', 'glassy_on.wav', 'glassy_off.wav'],
    ['Soft marimba', 'marimba_on.wav', 'marimba_off.wav'],
    ['Airy pluck', 'pluck_on.wav', 'pluck_off.wav'],
  ];
  const items = rows.map(([name, on, off]) => `
    <section>
      <h2>${name}</h2>
      <button data-src="${on}">Enabled</button>
      <button data-src="${off}">Disabled</button>
      <button data-both="${on}|${off}">Both</button>
    </section>`).join('');
  return `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wind chime candidates</title>
<style>
  :root{color-scheme:light dark;--bg:#fff;--fg:#1b1b1f;--card:#f3f3f6;--line:#d8d8e0;--acc:#2f6fed}
  @media (prefers-color-scheme:dark){:root{--bg:#16161a;--fg:#ececf1;--card:#212127;--line:#34343c;--acc:#7aa2ff}}
  body{margin:0;padding:24px 16px;background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,sans-serif}
  main{max-width:560px;margin:0 auto}
  p{opacity:.75}
  section{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 16px;margin:12px 0}
  h2{font-size:1rem;margin:0 0 10px}
  button{font:inherit;color:var(--fg);background:transparent;border:1px solid var(--line);border-radius:8px;padding:6px 14px;margin-right:8px;cursor:pointer}
  button:hover{border-color:var(--acc)}
  button:focus-visible{outline:2px solid var(--acc);outline-offset:2px}
</style></head><body><main>
<h1>Wind chime candidates</h1>
<p>Enabled is two soft notes rising, Disabled is the same notes falling. The chosen pair is copied to assets/sounds/chime_on.wav and chime_off.wav.</p>${items}
</main>
<script>
  let a;
  function play(src){ if(a){a.pause();} a=new Audio(src); a.play(); return a; }
  document.querySelectorAll('button').forEach(b=>b.addEventListener('click',()=>{
    if(b.dataset.src){ play(b.dataset.src); }
    else { const [x,y]=b.dataset.both.split('|'); play(x).addEventListener('ended',()=>setTimeout(()=>play(y),350)); }
  }));
</script></body></html>
`;
}

if (process.argv.includes('--check')) process.exit(check() ? 0 : 1);

mkdirSync(CAND, { recursive: true });
const write = (file, set, rising, peakDb) => {
  const n = rising ? set.notes : [set.notes[1], set.notes[0]];
  writeFileSync(file, wav(synth(set, n, peakDb)));
};
write(resolve(SOUNDS, 'chime_on.wav'), SETS.default, true, -6);
write(resolve(SOUNDS, 'chime_off.wav'), SETS.default, false, -8);
for (const k of ['glassy', 'marimba', 'pluck']) {
  write(resolve(CAND, `${k}_on.wav`), SETS[k], true, -6);
  write(resolve(CAND, `${k}_off.wav`), SETS[k], false, -8);
}
writeFileSync(resolve(CAND, 'index.html'), page());
process.exit(check() ? 0 : 1);
