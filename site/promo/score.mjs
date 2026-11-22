#!/usr/bin/env node
// Original soundtrack for the Guarana promo, synthesized in plain JavaScript: oscillators, envelopes,
// noise, biquad filters and a simple delay and reverb. No samples, no melody: a sub-bass heartbeat,
// risers into every cut, impacts on the beats, a driving pulse, and a lift at the call to action.
// Chords are only pad and bass roots; the arpeggio plays chord tones, nothing that resembles a tune.
//
// The cue list is site/promo/cues.json, the same file promo.js draws from, so cuts land on hits.
// Deterministic: a seeded generator, no Math.random, so every machine writes the same WAV.
//
//   node site/promo/score.mjs --out <dir>
// writes, 48 kHz stereo 16-bit:
//   guarana-score.wav          the full 20 s
//   guarana-score-15s.wav      the "cut15" segments of cues.json joined (the 15 s vertical cut)
//   guarana-score-teaser.wav   the "teaser" window (loops cut to cut)

import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const outDir = resolve(args[args.indexOf('--out') + 1] || '.');
const cues = JSON.parse(await readFile(join(HERE, 'cues.json'), 'utf8'));

const SR = 48000;
const DUR = cues.duration;
const N = Math.round(SR * DUR);
const TAU = Math.PI * 2;
const BEAT = 60 / cues.bpm;

// ---------------------------------------------------------------- tools
function mulberry(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const rnd = mulberry(0xc0ffee);
const noise = () => rnd() * 2 - 1;
const clamp = (x, a = 0, b = 1) => (x < a ? a : x > b ? b : x);
const lerp = (a, b, p) => a + (b - a) * p;
const hash = (x) => { const r = mulberry(Math.round(x * 1000) + 7); return r(); };

class Bq {
  constructor() { this.x1 = this.x2 = this.y1 = this.y2 = 0; this.b0 = 1; this.b1 = this.b2 = this.a1 = this.a2 = 0; }
  set(type, f, q = 0.707) {
    f = clamp(f, 10, SR * 0.45);
    const w = (TAU * f) / SR;
    const c = Math.cos(w);
    const al = Math.sin(w) / (2 * q);
    let b0;
    let b1;
    let b2;
    if (type === 'lp') { b0 = (1 - c) / 2; b1 = 1 - c; b2 = b0; } else if (type === 'hp') { b0 = (1 + c) / 2; b1 = -(1 + c); b2 = b0; } else { b0 = al; b1 = 0; b2 = -al; }
    const a0 = 1 + al;
    this.b0 = b0 / a0; this.b1 = b1 / a0; this.b2 = b2 / a0; this.a1 = (-2 * c) / a0; this.a2 = (1 - al) / a0;
    return this;
  }
  p(x) {
    const y = this.b0 * x + this.b1 * this.x1 + this.b2 * this.x2 - this.a1 * this.y1 - this.a2 * this.y2;
    this.x2 = this.x1; this.x1 = x; this.y2 = this.y1; this.y1 = y;
    return y;
  }
}

// ---------------------------------------------------------------- buses
const mkBus = () => new Float32Array(N);
const dryL = mkBus(); const dryR = mkBus(); // drums, fx, impacts
const bedL = mkBus(); const bedR = mkBus(); // pad, bass, arp: ducked by the kick
const revIn = mkBus();
const dlyIn = mkBus();
const kickTimes = [];

// Mix a mono voice into the buses at time t0. o: g (gain), pan -1..1, rev, dly (sends), bed.
function put(t0, a, o = {}) {
  const i0 = Math.round(t0 * SR);
  const pan = o.pan || 0;
  const g = o.g === undefined ? 1 : o.g;
  const gl = Math.cos(((pan + 1) * Math.PI) / 4) * g;
  const gr = Math.sin(((pan + 1) * Math.PI) / 4) * g;
  const L = o.bed ? bedL : dryL;
  const R = o.bed ? bedR : dryR;
  const rev = o.rev || 0;
  const dly = o.dly || 0;
  for (let i = 0; i < a.length; i++) {
    const k = i0 + i;
    if (k < 0) continue;
    if (k >= N) break;
    const x = a[i];
    L[k] += x * gl;
    R[k] += x * gr;
    if (rev) revIn[k] += x * rev * g;
    if (dly) dlyIn[k] += x * dly * g;
  }
}
const arr = (sec) => new Float32Array(Math.max(1, Math.round(sec * SR)));
const sat = (x, d = 1) => Math.tanh(x * d);

// ---------------------------------------------------------------- voices
function heart(t, v, low) {
  const a = arr(0.5);
  let ph = 0;
  for (let i = 0; i < a.length; i++) {
    const tt = i / SR;
    const f = (low ? 34 : 38) + (low ? 22 : 26) * Math.exp(-tt * 16);
    ph += (TAU * f) / SR;
    const env = (1 - Math.exp(-tt * 700)) * Math.exp(-tt * (low ? 9 : 7));
    const x = Math.sin(ph) + 0.45 * Math.sin(ph * 2 + 0.3) * Math.exp(-tt * 18) + 0.18 * Math.sin(ph * 3) * Math.exp(-tt * 26);
    a[i] = sat(x * env * 1.6, 1.2) * 0.9;
  }
  put(t, a, { g: v * 0.95, rev: 0.06 });
}
function kick(t, v) {
  kickTimes.push({ t, v });
  const a = arr(0.42);
  let ph = 0;
  for (let i = 0; i < a.length; i++) {
    const tt = i / SR;
    ph += (TAU * (44 + 130 * Math.exp(-tt * 30))) / SR;
    const x = Math.sin(ph) * Math.exp(-tt * 8.5) + (tt < 0.004 ? noise() * 0.5 : 0);
    a[i] = sat(x, 1.5);
  }
  put(t, a, { g: v * 0.9 });
}
function boom(t, v) {
  kickTimes.push({ t, v: v * 1.2 });
  const n = arr(2.4);
  let ph = 0;
  const bp = new Bq();
  for (let i = 0; i < n.length; i++) {
    const tt = i / SR;
    ph += (TAU * (38 + 70 * Math.exp(-tt * 9))) / SR;
    let x = Math.sin(ph) * Math.exp(-tt * 1.9) * 1.1;
    x += Math.sin(ph * 2.01) * 0.22 * Math.exp(-tt * 5);
    if (i % 32 === 0) bp.set('bp', 5200 * Math.exp(-tt * 4) + 220, 0.9);
    x += bp.p(noise()) * 0.55 * Math.exp(-tt * 5.5);
    if (tt < 0.003) x += noise() * 0.9;
    for (const [f, lv] of [[181, 0.1], [267, 0.08], [411, 0.06], [623, 0.04]]) x += Math.sin(TAU * f * tt) * lv * Math.exp(-tt * 6);
    n[i] = sat(x, 1.3);
  }
  put(t, n, { g: v * 0.95, rev: 0.5 });
}
function riser(t0, t1, v) {
  const dur = t1 - t0;
  const n = arr(dur);
  const bp = new Bq();
  const lp = new Bq();
  let ph = 0;
  for (let i = 0; i < n.length; i++) {
    const tt = i / SR;
    const p = tt / dur;
    const rise = Math.pow(p, 1.7);
    if (i % 32 === 0) {
      bp.set('bp', 260 * Math.pow(30, p), 1.1);
      lp.set('lp', 500 + 3500 * p, 0.8);
    }
    const f = 110 * Math.pow(8, p);
    ph += (TAU * f) / SR;
    const saw = lp.p(((ph / TAU) % 1) * 2 - 1);
    const gap = clamp((dur - tt) / 0.03);
    n[i] = (bp.p(noise()) * 0.9 + saw * 0.3 * Math.sin(TAU * 6 * tt + 1) * 0.5 + saw * 0.22) * rise * gap;
  }
  put(t0, n, { g: v * 0.5, rev: 0.35, pan: 0 });
}
function hit(t, v) {
  const a = arr(0.4);
  const bp = new Bq().set('bp', 1900, 0.8);
  let ph = 0;
  for (let i = 0; i < a.length; i++) {
    const tt = i / SR;
    ph += (TAU * (52 + 80 * Math.exp(-tt * 25))) / SR;
    a[i] = sat(Math.sin(ph) * Math.exp(-tt * 11) * 1.1 + bp.p(noise()) * 0.5 * Math.exp(-tt * 40), 1.3);
  }
  put(t, a, { g: v * 0.55, rev: 0.28 });
}
function tick(t, v) {
  const a = arr(0.16);
  for (let i = 0; i < a.length; i++) {
    const tt = i / SR;
    a[i] = Math.sin(TAU * 1760 * tt) * Math.exp(-tt * 55) + 0.45 * Math.sin(TAU * 3520 * tt) * Math.exp(-tt * 90);
  }
  put(t, a, { g: v * 0.3, pan: (hash(t) - 0.5) * 0.8, dly: 0.3, rev: 0.2 });
}
function glitch(t, v) {
  const a = arr(0.22);
  let held = 0;
  let ph = 0;
  for (let i = 0; i < a.length; i++) {
    const tt = i / SR;
    if (i % 28 === 0) held = noise();
    ph += (TAU * (30 + 45 * Math.exp(-tt * 18))) / SR;
    const q = Math.round((held * 0.6 + Math.sin(ph) * 0.9) * 3) / 3;
    a[i] = q * Math.exp(-tt * 13);
  }
  put(t, a, { g: v * 0.5, pan: (hash(t) - 0.5) * 0.6 });
}
function click(t, v) {
  const a = arr(0.1);
  let prev = 0;
  for (let i = 0; i < a.length; i++) {
    const tt = i / SR;
    const n = noise();
    a[i] = (n - prev) * 0.5 * Math.exp(-tt * 500) + Math.sin(TAU * 1100 * tt) * 0.5 * Math.exp(-tt * 70);
    prev = n;
  }
  put(t, a, { g: v * 0.5, rev: 0.12 });
}
function key(t, v) {
  const a = arr(0.06);
  const bp = new Bq().set('bp', 3000, 1.2);
  for (let i = 0; i < a.length; i++) {
    const tt = i / SR;
    a[i] = bp.p(noise()) * 0.7 * Math.exp(-tt * 260) + Math.sin(TAU * 240 * tt) * 0.25 * Math.exp(-tt * 110);
  }
  put(t, a, { g: v * 0.4, pan: (hash(t * 3) - 0.5) * 0.5 });
}
function chime(t, v) {
  const a = arr(3.4);
  for (const f of [523.25, 659.25, 783.99, 987.77]) {
    for (let i = 0; i < a.length; i++) {
      const tt = i / SR;
      const idx = 2.2 * Math.exp(-tt * 3);
      a[i] += Math.sin(TAU * f * tt + idx * Math.sin(TAU * f * 3.5 * tt)) * Math.exp(-tt * 1.5) * (1 - Math.exp(-tt * 900)) * 0.22;
    }
  }
  put(t, a, { g: v * 0.55, rev: 0.7, dly: 0.2 });
}
function lock(t, v) {
  const a = arr(1.2);
  let ph = 0;
  let ph2 = 0;
  const hp = new Bq().set('hp', 6000, 0.7);
  for (let i = 0; i < a.length; i++) {
    const tt = i / SR;
    ph += (TAU * (46 + 60 * Math.exp(-tt * 22))) / SR;
    ph2 += (TAU * lerp(880, 1760, 1 - Math.exp(-tt * 40))) / SR;
    a[i] = sat(Math.sin(ph) * Math.exp(-tt * 7) * 1.2, 1.3) + Math.sin(ph2) * 0.35 * Math.exp(-tt * 4.5) + hp.p(noise()) * 0.3 * Math.exp(-tt * 9);
  }
  kickTimes.push({ t, v });
  put(t, a, { g: v * 0.7, rev: 0.45 });
}
function countUp(t0, t1, v) {
  let t = t0;
  let k = 0;
  const total = 14;
  while (t < t1 - 0.02) {
    const p = (t - t0) / (t1 - t0);
    const f = 500 * Math.pow(4.8, p);
    const a = arr(0.08);
    for (let i = 0; i < a.length; i++) { const tt = i / SR; a[i] = Math.sin(TAU * f * tt) * Math.exp(-tt * 60) + 0.3 * Math.sin(TAU * f * 2 * tt) * Math.exp(-tt * 90); }
    put(t, a, { g: v * 0.3, pan: Math.sin(k) * 0.4, dly: 0.25 });
    t += lerp(0.115, 0.045, p);
    k++;
  }
  void total;
}
function snare(t, v) {
  const a = arr(0.3);
  const bp = new Bq().set('bp', 1900, 0.7);
  let ph = 0;
  for (let i = 0; i < a.length; i++) {
    const tt = i / SR;
    ph += (TAU * (150 + 60 * Math.exp(-tt * 40))) / SR;
    a[i] = bp.p(noise()) * 0.8 * Math.exp(-tt * 20) + Math.sin(ph) * 0.5 * Math.exp(-tt * 26);
  }
  put(t, a, { g: v * 0.55, rev: 0.22, pan: 0.05 });
}
function hat(t, v, open) {
  const a = arr(open ? 0.3 : 0.07);
  const hp = new Bq().set('hp', 7500, 0.7);
  for (let i = 0; i < a.length; i++) {
    const tt = i / SR;
    a[i] = hp.p(noise()) * Math.exp(-tt * (open ? 16 : 75));
  }
  put(t, a, { g: v * 0.22, pan: 0.25 * (hash(t) - 0.5) });
}
function bass(t, f, dur, v) {
  const a = arr(dur + 0.12);
  const lp = new Bq();
  let ph = 0;
  for (let i = 0; i < a.length; i++) {
    const tt = i / SR;
    ph += (TAU * f) / SR;
    if (i % 16 === 0) lp.set('lp', 110 + 800 * Math.exp(-tt * 12), 1.1);
    const saw = lp.p(((ph / TAU) % 1) * 2 - 1);
    const env = (1 - Math.exp(-tt * 500)) * (tt < dur ? 1 : Math.exp(-(tt - dur) * 60));
    a[i] = sat((saw * 0.55 + Math.sin(ph) * 0.7) * env, 1.2);
  }
  put(t, a, { g: v * 0.6, bed: true });
}
function pluck(t, f, v, pan) {
  const a = arr(0.45);
  const lp = new Bq();
  let p1 = 0;
  let p2 = 0;
  for (let i = 0; i < a.length; i++) {
    const tt = i / SR;
    p1 += (TAU * f * 1.004) / SR;
    p2 += (TAU * f * 0.996) / SR;
    if (i % 16 === 0) lp.set('lp', 500 + 3600 * Math.exp(-tt * 11), 1.3);
    a[i] = lp.p((((p1 / TAU) % 1) + ((p2 / TAU) % 1)) - 1) * Math.exp(-tt * 7) * (1 - Math.exp(-tt * 800));
  }
  put(t, a, { g: v * 0.3, pan, bed: true, dly: 0.5, rev: 0.25 });
}
function pad(t, dur, freqs, v, bright, swell = 0.5) {
  const a = arr(dur + 0.6);
  const lp = new Bq();
  const phs = freqs.flatMap(() => [rnd() * TAU, rnd() * TAU, rnd() * TAU]);
  for (let i = 0; i < a.length; i++) {
    const tt = i / SR;
    let x = 0;
    freqs.forEach((f, k) => {
      for (let j = 0; j < 3; j++) {
        const fd = f * Math.pow(2, ((j - 1) * 7) / 1200);
        phs[k * 3 + j] += (TAU * fd) / SR;
        x += ((phs[k * 3 + j] / TAU) % 1) * 2 - 1;
      }
    });
    if (i % 64 === 0) lp.set('lp', bright * (0.7 + 0.3 * Math.sin(tt * 1.3)) * (0.5 + 0.5 * clamp(tt / (dur * 0.6))), 0.9);
    const env = Math.min(1, tt / swell) * (tt < dur ? 1 : Math.exp(-(tt - dur) * 5));
    a[i] = lp.p(x / (freqs.length * 3)) * env;
  }
  put(t, a, { g: v * 0.9, bed: true, rev: 0.5, pan: 0 });
}
function drone(t0, t1, v) {
  const a = arr(t1 - t0);
  let p1 = 0;
  let p2 = 0;
  const lp = new Bq().set('lp', 260, 1.2);
  for (let i = 0; i < a.length; i++) {
    const tt = i / SR;
    const wob = 1 + 0.012 * Math.sin(tt * 5.1) + 0.02 * Math.sin(tt * 1.7);
    p1 += (TAU * 55 * wob) / SR;
    p2 += (TAU * 82.4 * wob * 1.003) / SR;
    a[i] = lp.p((((p1 / TAU) % 1) * 2 - 1) * 0.6 + (((p2 / TAU) % 1) * 2 - 1) * 0.35) * Math.min(1, tt / 0.3);
  }
  put(t0, a, { g: v, bed: true, rev: 0.2 });
}

// ---------------------------------------------------------------- arrangement from the cue list
const ev = (n) => cues.events.find((e) => e.n === n);
for (const e of cues.events) {
  if (e.k === 'heart') heart(e.t, e.v, e.n.endsWith('b'));
  else if (e.k === 'boom') boom(e.t, e.v);
  else if (e.k === 'riser') riser(e.t, e.t1, e.v);
  else if (e.k === 'hit') hit(e.t, e.v);
  else if (e.k === 'tick') tick(e.t, e.v);
  else if (e.k === 'glitch') glitch(e.t, e.v);
  else if (e.k === 'click') click(e.t, e.v);
  else if (e.k === 'chime') chime(e.t, e.v);
  else if (e.k === 'lock') lock(e.t, e.v);
  else if (e.k === 'count') countUp(e.t, e.t1, e.v);
  else if (e.k === 'key') {
    const n = 22;
    for (let i = 0; i < n; i++) key(e.t + (i * (e.t1 - e.t)) / n, e.v);
  }
}

const inR = (t, a, b) => t >= a - 1e-9 && t < b - 1e-9;
const CH = [
  { root: 55, tones: [220, 261.63, 329.63] }, // Am
  { root: 43.65, tones: [174.61, 220, 261.63] }, // F
  { root: 65.41, tones: [261.63, 329.63, 392] }, // C
  { root: 49, tones: [196, 246.94, 293.66] }, // G
];
const chordAt = (t) => CH[Math.floor(t / 2) % 4];
const stopKicks = [[1.9, 2], [3.75, 4], [7.5, 8], [10.5, 11], [13.25, 13.5], [16.25, 16.5], [17.75, 18]]; // gaps before each cut
const gapped = (t) => stopKicks.some(([a, b]) => t >= a - 1e-9 && t < b - 1e-9);

// the pulse: four on the floor from the fix to the end of the legacy beat
for (let b = 0; b < DUR / BEAT; b++) {
  const t = b * BEAT;
  if (gapped(t)) continue;
  let kv = 0;
  if (inR(t, 4.5, 7.5)) kv = 0.55 + 0.1 * (t - 4);
  else if (inR(t, 8.5, 10.5)) kv = 0.85;
  else if (inR(t, 11.5, 13.25)) kv = 0.55;
  else if (inR(t, 13.5, 16.25)) kv = 0.9;
  else if (inR(t, 16.5, 17.75)) kv = 1;
  else if (inR(t, 18.5, 19.5)) kv = 0.5;
  if (kv) kick(t, kv);
}
// the problem scene: the pulse limps, off the grid, then drops out
for (const t of [2.5, 2.875, 3.25, 3.625]) kick(t, 0.5);

// off-beat hats and sixteenths
for (let s = 0; s < DUR / (BEAT / 4); s++) {
  const t = s * (BEAT / 4);
  if (gapped(t)) continue;
  const off = s % 4 === 2;
  const sx = s % 2 === 1;
  if (inR(t, 5, 7.5) && off) hat(t, 0.8, s % 8 === 2);
  if (inR(t, 8.5, 10.5) && (off || (sx && inR(t, 9.5, 10.5)))) hat(t, off ? 0.9 : 0.5, s % 8 === 2);
  if (inR(t, 11, 13.25) && s % 4 !== 0) hat(t, 0.35, false);
  if (inR(t, 13.5, 16.25)) hat(t, off ? 0.9 : sx ? 0.45 : 0.25, s % 8 === 2);
  if (inR(t, 16.5, 17.75)) hat(t, off ? 1 : 0.5, false);
}
// snares: backbeat in the loud parts, rolls into the last cuts
for (let b = 0; b < DUR / BEAT; b++) {
  const t = b * BEAT;
  if (b % 2 === 1) {
    if (inR(t, 8.5, 10.5) || inR(t, 14, 16.25) || inR(t, 16.5, 17.5)) snare(t, 0.8);
  }
}
for (let s = 0; s < 8; s++) snare(7.5 + s * 0.0625, 0.25 + 0.08 * s); // 7.5 to 8.0 roll, speeding up
for (let s = 0; s < 8; s++) snare(10.5 + s * 0.0625, 0.25 + 0.08 * s);
for (let s = 0; s < 8; s++) snare(17.5 + s * 0.0625, 0.3 + 0.1 * s);

// bass: root notes on eighths in the driving parts
for (let s = 0; s < DUR / (BEAT / 2); s++) {
  const t = s * (BEAT / 2);
  if (gapped(t)) continue;
  const c = chordAt(t);
  let v = 0;
  if (inR(t, 5, 7.5) || inR(t, 8.5, 10.5) || inR(t, 13.5, 16.25) || inR(t, 16.5, 17.75)) v = 0.9;
  else if (inR(t, 11, 13.25) && s % 2 === 0) v = 0.6;
  if (!v) continue;
  const hi = s % 4 === 3 || s % 8 === 6;
  bass(t, c.root * (hi ? 2 : 1), BEAT / 2 - 0.04, v);
}
// plucks (chord tones, sixteenths) in the frame-generation beat, the device beat and the AI beat
for (let s = 0; s < DUR / (BEAT / 4); s++) {
  const t = s * (BEAT / 4);
  if (gapped(t)) continue;
  const on = inR(t, 6, 7.5) || inR(t, 8.5, 10.5) || inR(t, 14.5, 16.25) || inR(t, 16.5, 17.5);
  if (!on) continue;
  const c = chordAt(t);
  const seq = [0, 1, 2, 1, 2, 1, 0, 1];
  const f = c.tones[seq[s % 8]] * (s % 16 >= 8 ? 2 : 1);
  pluck(t, f, s % 4 === 0 ? 0.9 : 0.55, Math.sin(s) * 0.5);
}

// pads: a drone for the problem, warm chords after, and the lift at the end
drone(2.0, 4.0, 0.5);
for (let bar = 2; bar < 9; bar++) {
  const t = bar * 2;
  const c = chordAt(t);
  const f = c.tones;
  if (bar === 2 || bar === 3) pad(t, 2, f, 0.55, 1800, 0.4); // the fix
  else if (bar === 4) pad(t, 2, f, 0.5, 1600, 0.5);
  else if (bar === 5) pad(t, 2, f, 0.45, 1400, 0.5);
  else if (bar === 6 || bar === 7) pad(t - 0.5 + 0.5, 2, f, 0.6, 2200, 0.35);
  else if (bar === 8) pad(t, 2, f, 0.55, 2200, 0.35);
}
// the lift at the call to action: F then C, bright, with a rising sparkle
pad(18, 1.0, [174.61, 220, 261.63, 329.63, 523.25], 0.9, 4200, 0.12);
pad(19, 1.4, [130.81, 261.63, 329.63, 392, 523.25, 783.99], 1.0, 5200, 0.12);
bass(18, 43.65, 0.9, 1);
bass(19, 65.41, 1.2, 1);
{
  // sparkle: bell tones on chord tones, rising, no pattern that repeats like a tune
  const tones = [523.25, 659.25, 783.99, 1046.5, 1318.5, 1568];
  for (let i = 0; i < 12; i++) {
    const t = 18.5 + i * 0.125;
    const f = tones[(i * 5 + 2) % tones.length];
    const a = arr(0.5);
    for (let k = 0; k < a.length; k++) { const tt = k / SR; a[k] = Math.sin(TAU * f * tt + 1.2 * Math.sin(TAU * f * 2 * tt) * Math.exp(-tt * 8)) * Math.exp(-tt * 7); }
    put(t, a, { g: 0.12 + 0.012 * i, pan: Math.sin(i * 1.7) * 0.7, dly: 0.5, rev: 0.5 });
  }
}

// ---------------------------------------------------------------- mix
// Kick ducking for the bed.
const duck = new Float32Array(N).fill(1);
for (const k of kickTimes) {
  const i0 = Math.round(k.t * SR);
  const depth = Math.min(0.75, 0.5 * k.v);
  for (let i = 0; i < SR * 0.3; i++) {
    const j = i0 + i;
    if (j >= N) break;
    const d = 1 - depth * Math.exp(-i / (SR * 0.1)) * (i < SR * 0.004 ? i / (SR * 0.004) : 1);
    if (d < duck[j]) duck[j] = d;
  }
}
// Reverb: four combs and two allpasses per side, with damping.
function reverb(input, seedShift) {
  const combs = [1557, 1617, 1491, 1422].map((d) => ({ d: d + seedShift, buf: new Float32Array(d + seedShift), i: 0, lp: 0 }));
  const aps = [556, 441].map((d) => ({ d: d + seedShift, buf: new Float32Array(d + seedShift), i: 0 }));
  const out = new Float32Array(N);
  for (let n = 0; n < N; n++) {
    const x = input[n] * 0.2;
    let y = 0;
    for (const c of combs) {
      const v = c.buf[c.i];
      c.lp = v * 0.72 + c.lp * 0.28;
      c.buf[c.i] = x + c.lp * 0.84;
      c.i = (c.i + 1) % c.d;
      y += v;
    }
    for (const a of aps) {
      const v = a.buf[a.i];
      const z = y + v * -0.5;
      a.buf[a.i] = z;
      y = v + z * 0.5;
      a.i = (a.i + 1) % a.d;
    }
    out[n] = y;
  }
  return out;
}
const revL = reverb(revIn, 0);
const revR = reverb(revIn, 23);
// Ping-pong delay at three eighths.
const dL = new Float32Array(N);
const dR = new Float32Array(N);
{
  const d = Math.round(BEAT * 0.75 * SR);
  const lpL = new Bq().set('lp', 3200, 0.7);
  const lpR = new Bq().set('lp', 3200, 0.7);
  for (let n = 0; n < N; n++) {
    const inL = dlyIn[n] + (n >= d ? dR[n - d] * 0.4 : 0);
    const inR2 = n >= d ? dL[n - d] * 0.4 : 0;
    dL[n] = lpL.p(inL);
    dR[n] = lpR.p(inR2);
  }
}
const mixL = new Float32Array(N);
const mixR = new Float32Array(N);
for (let n = 0; n < N; n++) {
  mixL[n] = dryL[n] + bedL[n] * duck[n] + revL[n] * 0.55 + dL[n] * 0.5;
  mixR[n] = dryR[n] + bedR[n] * duck[n] + revR[n] * 0.55 + dR[n] * 0.5;
}
// Master: remove DC and rumble, glue with a soft clip, fade the tail, normalise to -3 dBFS.
{
  const hpL = new Bq().set('hp', 24, 0.7);
  const hpR = new Bq().set('hp', 24, 0.7);
  for (let n = 0; n < N; n++) {
    let l = sat(hpL.p(mixL[n]), 0.9);
    let r = sat(hpR.p(mixR[n]), 0.9);
    const fin = clamp(n / (SR * 0.004));
    const fout = clamp((N - n) / (SR * 0.35));
    const g = fin * fout * fout;
    l *= g;
    r *= g;
    mixL[n] = l;
    mixR[n] = r;
  }
  let peak = 0;
  for (let n = 0; n < N; n++) peak = Math.max(peak, Math.abs(mixL[n]), Math.abs(mixR[n]));
  const gain = Math.pow(10, -3 / 20) / peak;
  for (let n = 0; n < N; n++) { mixL[n] *= gain; mixR[n] *= gain; }
}

// ---------------------------------------------------------------- output
function wav(l, r) {
  const n = l.length;
  const buf = Buffer.alloc(44 + n * 4);
  buf.write('RIFF', 0);
  buf.writeUInt32LE(36 + n * 4, 4);
  buf.write('WAVEfmt ', 8);
  buf.writeUInt32LE(16, 16);
  buf.writeUInt16LE(1, 20);
  buf.writeUInt16LE(2, 22);
  buf.writeUInt32LE(SR, 24);
  buf.writeUInt32LE(SR * 4, 28);
  buf.writeUInt16LE(4, 32);
  buf.writeUInt16LE(16, 34);
  buf.write('data', 36);
  buf.writeUInt32LE(n * 4, 40);
  const dith = mulberry(99);
  for (let i = 0; i < n; i++) {
    const q = (x) => Math.max(-32768, Math.min(32767, Math.round(x * 32767 + (dith() - dith()) * 0.7)));
    buf.writeInt16LE(q(l[i]), 44 + i * 4);
    buf.writeInt16LE(q(r[i]), 46 + i * 4);
  }
  return buf;
}
// Join stretches of the score (with 6 ms fades at each join so nothing clicks).
function splice(segs) {
  const parts = segs.map(([a, b]) => {
    const i0 = Math.round(a * SR);
    const i1 = Math.round(b * SR);
    const l = mixL.slice(i0, i1);
    const r = mixR.slice(i0, i1);
    const f = Math.round(0.006 * SR);
    for (let i = 0; i < f && i < l.length; i++) {
      const g = i / f;
      if (a > 0) { l[i] *= g; r[i] *= g; }
      if (b < DUR) { l[l.length - 1 - i] *= g; r[r.length - 1 - i] *= g; }
    }
    return [l, r];
  });
  const total = parts.reduce((s, p) => s + p[0].length, 0);
  const L = new Float32Array(total);
  const R = new Float32Array(total);
  let o = 0;
  for (const [l, r] of parts) { L.set(l, o); R.set(r, o); o += l.length; }
  return [L, R];
}

await mkdir(outDir, { recursive: true });
await writeFile(join(outDir, 'guarana-score.wav'), wav(mixL, mixR));
await writeFile(join(outDir, 'guarana-score-15s.wav'), wav(...splice(cues.cut15)));
await writeFile(join(outDir, 'guarana-score-teaser.wav'), wav(...splice([[cues.teaser.start, cues.teaser.end]])));
let pk = 0;
let sq = 0;
for (let n = 0; n < N; n++) { pk = Math.max(pk, Math.abs(mixL[n]), Math.abs(mixR[n])); sq += mixL[n] * mixL[n] + mixR[n] * mixR[n]; }
console.log(`score: ${DUR} s, peak ${(20 * Math.log10(pk)).toFixed(1)} dBFS, rms ${(10 * Math.log10(sq / (2 * N))).toFixed(1)} dBFS -> ${outDir}`);
