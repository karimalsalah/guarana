/* Guarana promo: 20 seconds, one clock.
   renderAt(t) sets every animated property for time t (seconds) from scratch, so frames can be
   rendered in any order and come out identical. Autoplay is renderAt(wall clock) on each frame.
   Query: ?format=vertical|landscape  ?render=1 (exact size, no autoplay; used by render.mjs)
          ?t=<seconds> (one still frame)  ?debug=1 (time code)  ?play=1 (autoplay even when the
          system asks for reduced motion). While previewing: space pauses, arrow keys step one
          frame (with shift, one second). */
(function () {
  'use strict';

  const DURATION = 20;
  const FPS = 30;
  const SIZES = { vertical: [1080, 1920], landscape: [1920, 1080] };
  const query = new URLSearchParams(location.search);
  const asked = (query.get('format') || '').toLowerCase();
  const FORMAT = SIZES[asked] ? asked : (innerWidth > innerHeight ? 'landscape' : 'vertical');
  const W = SIZES[FORMAT][0];
  const H = SIZES[FORMAT][1];
  const VERTICAL = FORMAT === 'vertical';
  const RENDER = query.has('render');
  const STILL = query.has('t') ? Number(query.get('t')) : null;
  const SVGNS = 'http://www.w3.org/2000/svg';

  const $ = (id) => document.getElementById(id);
  const stage = $('stage');
  stage.classList.add('fmt-' + FORMAT);
  document.documentElement.classList.toggle('is-debug', query.has('debug'));
  document.documentElement.classList.toggle('is-render', RENDER);

  // ---------------------------------------------------------------- math
  const clamp = (x, a, b) => (x < a ? a : x > b ? b : x);
  const lerp = (a, b, p) => a + (b - a) * p;
  const seg = (t, a, b) => clamp((t - a) / (b - a), 0, 1);
  const bell = (p) => Math.sin(Math.PI * clamp(p, 0, 1));
  const fx = (v, n) => v.toFixed(n === undefined ? 2 : n);
  const ease = {
    inCubic: (p) => p * p * p,
    outCubic: (p) => 1 - Math.pow(1 - p, 3),
    inOutCubic: (p) => (p < 0.5 ? 4 * p * p * p : 1 - Math.pow(-2 * p + 2, 3) / 2),
    outExpo: (p) => (p >= 1 ? 1 : 1 - Math.pow(2, -10 * p)),
    inOutSine: (p) => -(Math.cos(Math.PI * p) - 1) / 2,
  };
  // Step response of a damped spring: 0 before dt = 0, settles at 1. freq in Hz, damping < 1.
  function spring(dt, freq, damping) {
    if (dt <= 0) return 0;
    const w = 2 * Math.PI * freq;
    const wd = w * Math.sqrt(1 - damping * damping);
    return 1 - Math.exp(-damping * w * dt) * (Math.cos(wd * dt) + ((damping * w) / wd) * Math.sin(wd * dt));
  }
  // Its exact derivative, per second.
  function springVel(dt, freq, damping) {
    if (dt <= 0) return 0;
    const w = 2 * Math.PI * freq;
    const wd = w * Math.sqrt(1 - damping * damping);
    return ((w * w) / wd) * Math.exp(-damping * w * dt) * Math.sin(wd * dt);
  }
  // Motion-blur sigma for a speed in px/s: half the distance covered in 1/60 s (a 180-degree shutter).
  const motionBlur = (pxPerSecond) => Math.min(34, Math.abs(pxPerSecond) / 60 / 2.4);
  function hash01(n) {
    let x = (n | 0) ^ 0x9e3779b9;
    x = Math.imul(x ^ (x >>> 16), 0x85ebca6b);
    x = Math.imul(x ^ (x >>> 13), 0xc2b2ae35);
    x ^= x >>> 16;
    return (x >>> 0) / 4294967296;
  }
  function rng(seed) {
    return () => {
      seed = (seed + 0x6d2b79f5) | 0;
      let x = Math.imul(seed ^ (seed >>> 15), 1 | seed);
      x = (x + Math.imul(x ^ (x >>> 7), 61 | x)) ^ x;
      return ((x ^ (x >>> 14)) >>> 0) / 4294967296;
    };
  }
  function mix(c1, c2, p) {
    const a = parseInt(c1.slice(1), 16);
    const b = parseInt(c2.slice(1), 16);
    const ch = (s) => Math.round(lerp((a >> s) & 255, (b >> s) & 255, p));
    return `rgb(${ch(16)}, ${ch(8)}, ${ch(0)})`;
  }
  function offsetWithin(el, ancestor) {
    let x = 0;
    let y = 0;
    for (let n = el; n && n !== ancestor; n = n.offsetParent) {
      x += n.offsetLeft;
      y += n.offsetTop;
    }
    return { x, y };
  }

  // ---------------------------------------------------------------- scenes and timing
  const scenes = Array.from(document.querySelectorAll('.scene')).map((el) => ({
    el,
    start: Number(el.dataset.start),
    end: Number(el.dataset.end),
    tag: el.dataset.tag || '',
  }));
  const sceneOf = (el) => scenes.find((s) => s.el.contains(el));
  // A cut sits between one scene's end and the next one's start (they overlap a little).
  const CUTS = scenes.slice(1).map((s, i) => Math.round(((scenes[i].end + s.start) / 2) * 100) / 100);
  const sceneAt = (t) => {
    let found = scenes[0];
    for (const s of scenes) if (t >= s.start) found = s;
    return found;
  };

  // ---------------------------------------------------------------- kinetic elements
  // Every [data-k] element enters at data-at (seconds) with its kind, and leaves at data-out
  // (default: its scene's end minus 0.3 s) with data-exit. All of it is a pure function of t.
  const kin = [];
  const defs = $('fx-defs');
  let filterSeq = 0;
  function blurFilter() {
    const f = document.createElementNS(SVGNS, 'filter');
    const id = 'mb' + filterSeq++;
    f.setAttribute('id', id);
    f.setAttribute('x', '-50%');
    f.setAttribute('y', '-50%');
    f.setAttribute('width', '200%');
    f.setAttribute('height', '200%');
    f.setAttribute('color-interpolation-filters', 'sRGB');
    const g = document.createElementNS(SVGNS, 'feGaussianBlur');
    g.setAttribute('stdDeviation', '0 0');
    f.appendChild(g);
    defs.appendChild(f);
    return { url: `url(#${id})`, g, last: '' };
  }
  function registerKinetics() {
    document.querySelectorAll('[data-k]').forEach((el, index) => {
      const d = el.dataset;
      const scene = sceneOf(el);
      const exit = d.exit || 'drift';
      kin.push({
        el,
        kind: d.k,
        at: Number(d.at),
        dur: d.dur ? Number(d.dur) : 0.55,
        out: exit === 'none' ? Infinity : d.out ? Number(d.out) : scene ? scene.end - 0.3 : Infinity,
        odur: d.odur ? Number(d.odur) : 0.3,
        exit,
        seed: index * 97 + 13,
        shine: el.classList.contains('ember'),
        filter: blurFilter(),
        h: 0,
      });
    });
  }
  // The clock a stuttering entrance sees: it advances in uneven steps with one long hitch.
  function hitchClock(dt) {
    const steps = [0.05, 0.05, 0.05, 0.22, 0.05, 0.05, 0.13, 0.05];
    let acc = 0;
    for (const s of steps) {
      if (dt < acc + s) return acc;
      acc += s;
    }
    return dt;
  }
  function enter(k, dt, st) {
    switch (k.kind) {
      case 'fade': {
        st.o = ease.outCubic(seg(dt, 0, k.dur));
        break;
      }
      case 'fade-up': {
        const p = ease.outCubic(seg(dt, 0, k.dur));
        st.o = p;
        st.y = (1 - p) * 44;
        st.bx = st.by = (1 - p) * 9;
        break;
      }
      case 'blur-in': {
        const p = ease.outCubic(seg(dt, 0, k.dur));
        st.o = p;
        st.s = lerp(1.06, 1, p);
        st.bx = st.by = (1 - p) * 22;
        break;
      }
      case 'rise': {
        const d = k.h * 1.18;
        st.y = (1 - spring(dt, 2.0, 0.78)) * d;
        st.by = motionBlur(springVel(dt, 2.0, 0.78) * d);
        st.o = dt > 0 ? 1 : 0;
        break;
      }
      case 'slide-l':
      case 'slide-r': {
        const dir = k.kind === 'slide-l' ? -1 : 1;
        const d = W * 0.46;
        st.x = dir * (1 - spring(dt, 1.9, 0.74)) * d;
        st.bx = motionBlur(springVel(dt, 1.9, 0.74) * d);
        st.o = clamp(dt / 0.07, 0, 1);
        break;
      }
      case 'slam': {
        st.s = lerp(1.85, 1, spring(dt, 2.6, 0.48));
        st.o = clamp(dt / 0.05, 0, 1);
        st.bx = st.by = motionBlur(springVel(dt, 2.6, 0.48) * k.h * 0.85) * 0.6;
        break;
      }
      case 'pop': {
        st.s = Math.max(0.001, spring(dt, 2.4, 0.52));
        st.o = clamp(dt / 0.08, 0, 1);
        break;
      }
      case 'stutter': {
        const q = hitchClock(dt);
        st.x = -(1 - ease.outCubic(seg(q, 0, 0.45))) * W * 0.24;
        st.o = dt > 0 ? 1 : 0;
        const after = seg(dt, 0.45, 1.3);
        if (after > 0 && after < 1) {
          const step = Math.floor(dt * 15);
          st.x += (hash01(step * 31 + k.seed) - 0.5) * 20 * (1 - after);
          st.y += (hash01(step * 17 + k.seed) - 0.5) * 9 * (1 - after);
        }
        break;
      }
      default:
        break;
    }
  }
  function leave(k, e, st) {
    switch (k.exit) {
      case 'left':
        st.x -= e * e * W * 0.5;
        st.bx += e * 26;
        st.o *= 1 - e * e;
        break;
      case 'right':
        st.x += e * e * W * 0.5;
        st.bx += e * 26;
        st.o *= 1 - e * e;
        break;
      case 'up':
        st.y -= e * e * (k.h || 120) * 1.4;
        st.by += e * 22;
        st.o *= 1 - e;
        break;
      case 'zoom':
        st.s *= 1 + e * 0.32;
        st.bx += e * 14;
        st.by += e * 14;
        st.o *= 1 - e;
        break;
      default: // drift
        st.y -= e * 34;
        st.bx += e * 9;
        st.by += e * 9;
        st.o *= 1 - e;
        break;
    }
  }
  function setBlur(k, bx, by) {
    let sx = Math.round(bx * 2) / 2;
    let sy = Math.round(by * 2) / 2;
    const off = sx < 0.5 && sy < 0.5;
    if (off) sx = sy = 0;
    const v = `${sx} ${sy}`;
    if (off) {
      k.el.style.filter = '';
      if (k.filter.last !== v) {
        k.filter.g.setAttribute('stdDeviation', v);
        k.filter.last = v;
      }
      return;
    }
    if (k.filter.last !== v) {
      k.filter.g.setAttribute('stdDeviation', v);
      k.filter.last = v;
    }
    k.el.style.filter = k.filter.url;
  }
  function applyKin(k, t) {
    const el = k.el;
    if (t < k.at || t > k.out + k.odur) {
      el.style.visibility = 'hidden';
      el.style.opacity = '0';
      el.style.transform = '';
      setBlur(k, 0, 0);
      if (k.shine) el.style.setProperty('--shine', '175%');
      return;
    }
    const st = { x: 0, y: 0, s: 1, o: 1, bx: 0, by: 0 };
    enter(k, t - k.at, st);
    if (t > k.out) leave(k, ease.inCubic(seg(t, k.out, k.out + k.odur)), st);
    el.style.visibility = st.o > 0.002 ? '' : 'hidden';
    el.style.opacity = fx(st.o, 4);
    el.style.transform = `translate3d(${fx(st.x)}px, ${fx(st.y)}px, 0) scale(${fx(st.s, 4)})`;
    setBlur(k, st.bx, st.by);
    if (k.shine) el.style.setProperty('--shine', fx(lerp(175, -75, ease.inOutSine(seg(t, k.at + 0.18, k.at + 1.1))), 1) + '%');
  }

  // ---------------------------------------------------------------- background, depth, light
  const bg = {
    world: $('world'),
    worldBg: $('world-bg'),
    orbs: [$('orb-a'), $('orb-b'), $('orb-c')],
    floor: $('floor'),
    beamA: $('beam-a'),
    beamB: $('beam-b'),
    flash: $('flash'),
    grain: $('grain'),
    lbTop: $('lb-top'),
    lbBot: $('lb-bot'),
    brand: $('chrome-brand'),
    tag: $('chrome-tag'),
    debug: $('debug'),
  };
  const particles = [];
  const grainTiles = [];
  function buildParticles() {
    const r = rng(20261113);
    const box = $('particles');
    const n = VERTICAL ? 34 : 42;
    for (let i = 0; i < n; i++) {
      const el = document.createElement('i');
      const depth = 0.35 + r() * 1.25;
      const size = (6 + r() * 30) * depth;
      el.style.width = el.style.height = fx(size, 1) + 'px';
      box.appendChild(el);
      particles.push({ el, x: r() * W, y: r() * H, depth, speed: 14 + r() * 26, phase: r() * 6.283, alpha: 0.05 + r() * 0.2 });
    }
  }
  function buildGrain() {
    const r = rng(99);
    const c = document.createElement('canvas');
    c.width = c.height = 256;
    const ctx = c.getContext('2d');
    for (let i = 0; i < 6; i++) {
      const img = ctx.createImageData(256, 256);
      for (let p = 0; p < img.data.length; p += 4) {
        const v = Math.floor(r() * 256);
        img.data[p] = img.data[p + 1] = img.data[p + 2] = v;
        img.data[p + 3] = 255;
      }
      ctx.putImageData(img, 0, 0);
      grainTiles.push(`url("${c.toDataURL('image/png')}")`);
    }
  }
  const SLAMS = () => kin.filter((k) => k.kind === 'slam' || k.kind === 'stutter').map((k) => k.at + 0.06);
  let slamTimes = [];
  function camera(t) {
    const sc = sceneAt(t);
    let z = 1 + 0.04 * ease.inOutSine(seg(t, sc.start, sc.end));
    let x = Math.sin(t * 0.37) * 12;
    let y = Math.cos(t * 0.29) * 9;
    for (const c of CUTS) {
      const d = t - c;
      if (d >= 0 && d < 0.7) z += 0.03 * Math.exp(-d * 8);
    }
    for (const s of slamTimes) {
      const d = t - s;
      if (d >= 0 && d < 0.5) {
        const amp = 15 * Math.exp(-d * 11);
        x += Math.sin(d * 75) * amp;
        y += Math.cos(d * 60) * amp * 0.6;
      }
    }
    return { x, y, z };
  }
  // Light sweeps: beam A crosses left to right, beam B right to left, each at its own cuts.
  const SWEEPS_A = [0.05, 4.4, 10.0, 14.8, 18.45];
  const SWEEPS_B = [2.0, 7.4, 12.6, 16.4];
  function sweep(el, list, t, forward, angle, width) {
    let p = -1;
    for (const s of list) if (t >= s - 0.12 && t <= s + 0.85) p = seg(t, s - 0.12, s + 0.85);
    el.style.width = width;
    if (p < 0) {
      el.style.opacity = '0';
      el.style.transform = 'translate3d(-200%, 0, 0)';
      return;
    }
    const e = ease.inOutSine(p);
    const x = forward ? lerp(-0.5 * W, 1.15 * W, e) : lerp(1.15 * W, -0.5 * W, e);
    el.style.opacity = fx(bell(p) * 0.95, 3);
    el.style.transform = `translate3d(${fx(x, 1)}px, 0, 0) rotate(${angle}deg)`;
    el.style.width = width;
  }
  function renderBackground(t, cam) {
    const dx = Math.sin(t * 0.21) * 0.12 * W;
    const dy = Math.cos(t * 0.17) * 0.08 * H;
    const warm = 0.75 + 0.25 * Math.sin(t * 0.9) + (t > 4.6 && t < 7.4 ? 0.25 : 0) + (t > 18 ? 0.45 : 0);
    bg.orbs[0].style.transform = `translate3d(${fx(W * 0.8 + dx + cam.x * 0.25, 1)}px, ${fx(H * 0.16 + dy + cam.y * 0.25, 1)}px, 0)`;
    bg.orbs[0].style.opacity = fx(clamp(warm, 0, 1.3) / 1.3, 3);
    bg.orbs[1].style.transform = `translate3d(${fx(W * 0.16 - dx * 0.7, 1)}px, ${fx(H * 0.74 - dy * 0.6, 1)}px, 0)`;
    bg.orbs[2].style.transform = `translate3d(${fx(W * 0.55 + Math.sin(t * 0.33) * 0.2 * W, 1)}px, ${fx(H * 0.5 + Math.cos(t * 0.29) * 0.1 * H, 1)}px, 0)`;
    bg.orbs[2].style.opacity = fx(0.5 + (t > 7.4 && t < 10 ? 0.5 * seg(t, 8.9, 9.4) : 0), 3);
    bg.floor.style.backgroundPosition = `0 ${fx((t * 96) % 128, 1)}px`;
    bg.floor.style.opacity = fx(0.4 + 0.2 * Math.sin(t * 0.6), 3);
    bg.worldBg.style.transform = `translate3d(${fx(-cam.x * 0.3, 1)}px, ${fx(-cam.y * 0.3, 1)}px, 0) scale(${fx(1.05 + 0.02 * Math.sin(t * 0.3), 4)})`;
    sweep(bg.beamA, SWEEPS_A, t, true, 16, '36%');
    sweep(bg.beamB, SWEEPS_B, t, false, -12, '22%');
    let flash = 0;
    for (const c of CUTS) if (t >= c) flash = 0.16 * Math.exp(-(t - c) * 13);
    if (t >= 18.0) flash = Math.max(flash, 0.22 * Math.exp(-(t - 18.0) * 6));
    bg.flash.style.opacity = fx(flash, 3);
    bg.grain.style.backgroundImage = grainTiles[Math.floor(t * 24) % grainTiles.length];
    const bars = VERTICAL ? 0 : 128 * (1 - ease.inOutCubic(seg(t, 1.45, 2.1)));
    bg.lbTop.style.height = bg.lbBot.style.height = fx(bars, 1) + 'px';
  }
  function renderParticles(t, cam) {
    const span = H + 160;
    for (const p of particles) {
      let y = (p.y - t * p.speed * p.depth) % span;
      if (y < 0) y += span;
      const x = p.x + Math.sin(t * 0.5 + p.phase) * 26 * p.depth + cam.x * p.depth * 0.6;
      p.el.style.transform = `translate3d(${fx(x, 1)}px, ${fx(y - 80 + cam.y * p.depth * 0.6, 1)}px, 0)`;
      p.el.style.opacity = fx(p.alpha * (0.6 + 0.4 * Math.sin(t * 1.3 + p.phase)), 3);
    }
  }
  function renderChrome(t) {
    bg.brand.style.opacity = fx(seg(t, 2.0, 2.3) * (1 - seg(t, 17.72, 17.98)), 3);
    const sc = sceneAt(t);
    bg.tag.textContent = sc.tag;
    bg.tag.style.opacity = sc.tag ? fx(seg(t, sc.start + 0.06, sc.start + 0.32) * (1 - seg(t, sc.end - 0.26, sc.end - 0.04)), 3) : '0';
  }

  // ---------------------------------------------------------------- 2 and 4: frame-time graphs
  const yOf = (ms) => 400 - ms * 6;
  function pathOf(pts) {
    let d = `M${fx(pts[0][0], 1)} ${fx(pts[0][1], 1)}`;
    for (let i = 1; i < pts.length; i++) d += `L${fx(pts[i][0], 1)} ${fx(pts[i][1], 1)}`;
    return d;
  }
  const gp = { line: $('gp-line'), glow: $('gp-glow'), head: $('gp-head'), w: 1000 };
  const gq = { line: $('gq-line'), glow: $('gq-glow'), head: $('gq-head'), target: $('gq-target'), lbl: $('gq-lbl'), w: VERTICAL ? 1000 : 1800 };
  function setupGraphs() {
    const svg = gq.line.ownerSVGElement;
    svg.setAttribute('viewBox', `0 0 ${gq.w} 440`);
    svg.querySelectorAll('line').forEach((l) => l.setAttribute('x2', gq.w));
  }
  // Previous build: frames held at 50 ms, with hitches on top (illustration, not data).
  function problemValue(s) {
    let v = 50 + (hash01(s * 3 + 11) - 0.5) * 2.2;
    if (hash01(s * 7 + 5) < 0.12) v += 7 + hash01(s * 13 + 1) * 13;
    return v;
  }
  function renderProblem(t) {
    const n = 56;
    const head = n + (t - 2.0) * 24;
    const base = Math.floor(head);
    const frac = head - base;
    const pts = [];
    for (let k = 0; k <= n + 1; k++) pts.push([((k - frac) / n) * gp.w, yOf(problemValue(base - n + k))]);
    const d = pathOf(pts);
    gp.line.setAttribute('d', d);
    gp.glow.setAttribute('d', d);
    const reveal = ease.outCubic(seg(t, 2.25, 2.9));
    gp.line.style.strokeDasharray = '1 1';
    gp.line.style.strokeDashoffset = fx(1 - reveal, 4);
    gp.glow.style.opacity = fx(0.26 * reveal, 3);
    const hy = lerp(pts[n][1], pts[n + 1][1], frac);
    gp.head.setAttribute('cy', fx(hy, 1));
    gp.head.style.opacity = reveal > 0.98 ? '1' : '0';
  }
  // Shown frames that last one or two refreshes in an uneven pattern, smoothed into a line that
  // breathes; then the presenter in progress holds every one for exactly 16.67 ms (illustration).
  const holds = (s) => (hash01(s * 5 + 3) < 0.42 ? 2 : 1);
  function renderPace(t) {
    const n = VERTICAL ? 40 : 66;
    const sub = 4;
    const rate = 20;
    const head = n + (t - 7.4) * rate;
    const lockAll = ease.inOutCubic(seg(t, 8.95, 9.4));
    const first = Math.floor((head - n) * sub);
    const pts = [];
    for (let k = 0; k <= (n + 1) * sub; k++) {
      const pos = (first + k) / sub;
      const s0 = Math.floor(pos);
      const m = lerp(holds(s0), holds(s0 + 1), ease.inOutSine(pos - s0));
      const ts = 7.4 + (pos - n) / rate;
      const lock = Math.max(lockAll, ease.inOutCubic(seg(ts, 8.35, 8.9)));
      const amp = 0.8 + 0.2 * Math.sin(ts * 3.1);
      const ms = lerp(16.67 + (m - 1) * 16.67 * amp, 16.67, lock);
      pts.push([((pos - (head - n)) / n) * gq.w, yOf(ms)]);
    }
    const d = pathOf(pts);
    const color = mix('#ff6b4a', '#3ccf7e', lockAll);
    gq.line.setAttribute('d', d);
    gq.glow.setAttribute('d', d);
    gq.line.style.stroke = color;
    gq.glow.style.stroke = color;
    gq.glow.style.opacity = fx(0.22 + 0.2 * lockAll, 3);
    gq.head.style.fill = color;
    let hy = yOf(16.67);
    for (let i = 1; i < pts.length; i++) {
      if (pts[i][0] >= gq.w) {
        const p = (gq.w - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
        hy = lerp(pts[i - 1][1], pts[i][1], p);
        break;
      }
    }
    gq.head.setAttribute('cx', gq.w);
    gq.head.setAttribute('cy', fx(hy, 1));
    gq.target.style.strokeOpacity = fx(0.55 + 0.45 * lockAll, 3);
    gq.target.style.strokeWidth = fx(3 + 2 * lockAll, 2);
  }

  // ---------------------------------------------------------------- 3: the fix
  const fix = { num: $('fps-num'), ms: $('draw-ms'), fill: $('draw-fill'), ghost: $('draw-ghost'), was: $('draw-was') };
  function renderFix(t) {
    const p = ease.inOutCubic(seg(t, 5.05, 6.15));
    fix.num.textContent = lerp(14.8, 22.8, p).toFixed(1);
    const land = t - 6.15;
    fix.num.style.filter = land > 0 && land < 0.9 ? `drop-shadow(0 0 ${fx(34 * Math.exp(-land * 4.5), 1)}px rgba(255, 190, 60, 0.85))` : '';
    const q = ease.inOutCubic(seg(t, 5.9, 6.7));
    const ms = lerp(15.7, 5.8, q);
    fix.ms.textContent = ms.toFixed(1);
    fix.fill.style.transform = `scaleX(${fx(ms / 15.7, 4)})`;
    fix.fill.style.filter = q > 0 ? `hue-rotate(${fx(q * 105, 1)}deg)` : '';
    fix.ghost.style.opacity = fx(seg(t, 5.9, 6.15), 3);
    fix.was.style.opacity = fx(seg(t, 6.6, 6.9), 3);
  }

  // ---------------------------------------------------------------- 5: frame generation
  const fg = { scene: document.querySelector('.s-fg'), track: $('fg-track'), cards: [], cw: 0, slot: 0, w: 0 };
  function buildFg() {
    const n = VERTICAL ? 10 : 14;
    for (let k = 0; k < n; k++) {
      const real = k % 3 === 0;
      const at = (i) => [28 + (i % 9) * 18, 74 - Math.sin((i % 9) * 0.5) * 16];
      const [bx, by] = at(k);
      const [gx, gy] = at(k - (k % 3));
      let motion = '';
      if (!real) {
        const ang = Math.atan2(by - gy, bx - gx);
        const ex = bx - Math.cos(ang) * 13;
        const ey = by - Math.sin(ang) * 13;
        const h1 = [ex - Math.cos(ang - 0.5) * 7, ey - Math.sin(ang - 0.5) * 7];
        const h2 = [ex - Math.cos(ang + 0.5) * 7, ey - Math.sin(ang + 0.5) * 7];
        motion = `<circle class="ghost" cx="${fx(gx, 1)}" cy="${fx(gy, 1)}" r="10"/>` +
          `<path class="mv" d="M${fx(gx + Math.cos(ang) * 11, 1)} ${fx(gy + Math.sin(ang) * 11, 1)}L${fx(ex, 1)} ${fx(ey, 1)}"/>` +
          `<path class="mv-head" d="M${fx(ex + Math.cos(ang) * 2, 1)} ${fx(ey + Math.sin(ang) * 2, 1)}L${fx(h1[0], 1)} ${fx(h1[1], 1)}L${fx(h2[0], 1)} ${fx(h2[1], 1)}Z"/>`;
      }
      const card = document.createElement('div');
      card.className = 'fg-card ' + (real ? 'real' : 'pred');
      card.innerHTML =
        '<svg viewBox="0 0 200 124" preserveAspectRatio="xMidYMid slice" focusable="false">' +
        '<rect class="sky" width="200" height="124"/>' +
        '<path class="hills" d="M0 100Q40 80 80 94T160 90T200 92V124H0Z"/>' +
        '<line class="ground" x1="0" y1="100" x2="200" y2="100"/>' +
        motion +
        `<circle class="ball" cx="${fx(bx, 1)}" cy="${fx(by, 1)}" r="10"/>` +
        '<rect class="hud" x="10" y="10" width="58" height="12" rx="6"/>' +
        '<rect class="hud-seg" x="13" y="13" width="14" height="6" rx="3"/><rect class="hud-seg" x="29" y="13" width="14" height="6" rx="3"/><rect class="hud-seg" x="45" y="13" width="14" height="6" rx="3"/>' +
        '<circle class="hud-map" cx="184" cy="18" r="9"/>' +
        `</svg><span class="fg-lbl">${real ? 'real' : 'predicted'}</span>`;
      fg.track.appendChild(card);
      fg.cards.push(card);
    }
  }
  function renderFg(t) {
    const scroll = (t - 10.0) * (VERTICAL ? 170 : 190);
    const nowX = fg.w * 0.5;
    const startX = nowX - fg.slot * 1.5;
    fg.cards.forEach((c, k) => {
      const x = startX + k * fg.slot - scroll;
      const a = 10.12 + k * 0.05;
      const y = (1 - spring(t - a, 2.1, 0.72)) * 120;
      const o = clamp((t - a) / 0.15, 0, 1);
      const hi = 1 - clamp(Math.abs(x + fg.cw / 2 - nowX) / (fg.slot * 0.55), 0, 1);
      c.style.transform = `translate3d(${fx(x, 1)}px, ${fx(y - hi * 12, 1)}px, 0) scale(${fx(1 + hi * 0.07, 4)})`;
      c.style.opacity = fx(o * (0.6 + 0.4 * hi), 3);
      c.style.setProperty('--hi', fx(hi, 3));
    });
  }

  // ---------------------------------------------------------------- 6: installer and doctor
  const app = { el: $('app'), btn: $('app-btn'), txt: $('app-btn-txt'), ripple: $('app-ripple'), prog: $('app-prog'), cursor: $('app-cursor'), rows: [] };
  function buildDoctor() {
    document.querySelectorAll('#app-list .app-row').forEach((li) => {
      const ic = li.querySelector('.ic');
      ic.innerHTML =
        '<svg viewBox="0 0 36 36" focusable="false">' +
        '<g class="g-tick"><circle class="ic-tick-bg" cx="18" cy="18" r="17"/><path class="ic-tick" pathLength="1" d="M10.5 18.5l5 5 10-11"/></g>' +
        '<path class="ic-spin" d="M18 4a14 14 0 0 1 14 14"/>' +
        '<path class="ic-warn" d="M16.2 8.5h3.6l-.6 12.5h-2.4zM18 24a2.3 2.3 0 1 1 0 4.6A2.3 2.3 0 0 1 18 24z"/></svg>';
      const vl = li.querySelector('.vl');
      app.rows.push({
        el: li,
        vl,
        ok: vl.dataset.ok,
        warnTxt: vl.dataset.warn || '',
        fix: li.dataset.fix === '1',
        tick: ic.querySelector('.g-tick'),
        tickPath: ic.querySelector('.ic-tick'),
        spin: ic.querySelector('.ic-spin'),
        warn: ic.querySelector('.ic-warn'),
      });
    });
  }
  function measureDoctor() {
    const b = offsetWithin(app.btn, app.el);
    app.bx = b.x + app.btn.offsetWidth * 0.56 - 9;
    app.by = b.y + app.btn.offsetHeight * 0.55 - 6;
    app.sx = app.el.offsetWidth * 0.86;
    app.sy = app.el.offsetHeight * 0.9;
  }
  function renderDoctor(t) {
    const c = ease.inOutCubic(seg(t, 12.74, 13.06));
    const press = t >= 13.08 && t < 13.2;
    app.cursor.style.opacity = fx(seg(t, 12.74, 12.84) * (1 - seg(t, 13.6, 13.8)), 3);
    app.cursor.style.transform = `translate3d(${fx(lerp(app.sx, app.bx, c), 1)}px, ${fx(lerp(app.sy, app.by, c), 1)}px, 0) scale(${press ? 0.86 : 1})`;
    const r = seg(t, 13.1, 13.54);
    app.ripple.style.opacity = r > 0 && r < 1 ? fx((1 - r) * 0.6, 3) : '0';
    app.ripple.style.transform = `scale(${fx(lerp(0.1, 3.4, ease.outCubic(r)), 3)})`;
    app.btn.style.transform = press ? 'scale(0.97)' : '';
    app.txt.textContent = t < 13.1 ? 'Install' : t < 13.72 ? 'Installing' : 'Ready to play';
    app.prog.style.transform = `scaleX(${fx(ease.inOutCubic(seg(t, 13.12, 13.72)), 4)})`;
    app.rows.forEach((row, i) => {
      const a = 13.22 + i * 0.14;
      const shown = ease.outCubic(seg(t, a, a + 0.18));
      const doneAt = a + (row.fix ? 0.44 : 0.22);
      const done = t >= doneAt;
      const working = t >= a && !done;
      const warning = row.fix && working && t >= a + 0.14;
      row.el.style.opacity = fx(shown, 3);
      row.el.style.transform = `translate3d(${fx((1 - shown) * 40, 1)}px, 0, 0)`;
      row.spin.style.opacity = working && !warning ? '1' : '0';
      row.spin.setAttribute('transform', `rotate(${fx(((t - a) * 720) % 360, 1)} 18 18)`);
      row.warn.style.opacity = warning ? '1' : '0';
      row.tick.style.opacity = done ? '1' : '0';
      row.tickPath.style.strokeDasharray = '1 1';
      row.tickPath.style.strokeDashoffset = fx(1 - ease.outCubic(seg(t, doneAt, doneAt + 0.16)), 3);
      row.vl.textContent = done ? row.ok : warning ? row.warnTxt : '';
      row.vl.classList.toggle('warn', warning);
    });
  }

  // ---------------------------------------------------------------- 7: controller
  const pad = {
    outline: $('pad-outline'),
    glow: $('pad-glow'),
    parts: $('pad-parts'),
    cable: $('pad-cable'),
    pulse: $('pad-pulse'),
    btns: Array.from(document.querySelectorAll('.pad-btn')),
  };
  function renderPad(t) {
    const draw = ease.inOutCubic(seg(t, 14.84, 15.42));
    pad.outline.style.strokeDashoffset = fx(1 - draw, 4);
    pad.parts.style.opacity = fx(ease.outCubic(seg(t, 15.12, 15.42)), 3);
    const drop = spring(t - 14.98, 2.2, 0.62);
    pad.cable.setAttribute('transform', `translate(0 ${fx((1 - drop) * -260, 1)})`);
    pad.cable.style.opacity = fx(seg(t, 14.95, 15.05), 3);
    const k = seg(t, 15.3, 15.85);
    pad.pulse.style.opacity = k > 0 && k < 1 ? fx((1 - k) * 0.9, 3) : '0';
    pad.pulse.setAttribute('r', fx(lerp(30, 230, ease.outCubic(k)), 1));
    const g = ease.outCubic(seg(t, 15.3, 15.6));
    pad.glow.style.opacity = fx(g, 3);
    pad.outline.style.opacity = fx(1 - g * 0.85, 3);
    pad.btns.forEach((b, i) => {
      const a = 15.55 + i * 0.09;
      const on = bell(seg(t, a, a + 0.32));
      b.style.fill = on > 0.01 ? `rgba(255, 201, 74, ${fx(on * 0.95, 3)})` : 'none';
    });
  }

  // ---------------------------------------------------------------- 8: AI agents
  const TICK = '<svg viewBox="0 0 32 32" focusable="false"><circle cx="16" cy="16" r="15" fill="rgba(60, 207, 126, 0.16)"/><path d="M9 16.5l4.5 4.5 9.5-10" fill="none" stroke="#3ccf7e" stroke-width="3.4" stroke-linecap="round" stroke-linejoin="round"/></svg>';
  const term = [];
  function buildTerm() {
    document.querySelectorAll('#term-body .tl').forEach((p) => {
      const tx = p.querySelector('.tx');
      const caret = document.createElement('span');
      caret.className = 'caret';
      tx.after(caret);
      const ok = p.querySelector('.ok');
      ok.innerHTML = TICK;
      term.push({ tx, caret, ok, text: tx.dataset.text });
    });
  }
  function renderAgents(t) {
    term.forEach((ln, i) => {
      const a = 16.6 + i * 0.18;
      const d = 0.16;
      ln.tx.textContent = ln.text.slice(0, Math.round(seg(t, a, a + d) * ln.text.length));
      const last = i === term.length - 1;
      const typing = t >= a && t < a + d + 0.05;
      const blink = last && t >= a + d && Math.floor(t * 3) % 2 === 0;
      ln.caret.style.display = typing || blink ? '' : 'none';
      const tickAt = a + d + 0.04;
      ln.ok.style.opacity = fx(clamp((t - tickAt) / 0.08, 0, 1), 3);
      ln.ok.style.transform = `scale(${fx(Math.max(0.001, spring(t - tickAt, 2.6, 0.55)), 3)})`;
    });
  }

  // ---------------------------------------------------------------- 9: logo reveal
  const logo = {
    drop: $('logo-drop'),
    leaf: $('logo-leaf'),
    speed: $('logo-speed'),
    echo: [$('echo-1'), $('echo-2'), $('echo-3')],
    blur: $('logo-mb-blur'),
    glow: $('logo-glow'),
    word: $('wordmark'),
  };
  function renderCta(t) {
    const a = 18.04;
    const dist = 70;
    const p = spring(t - a, 1.6, 0.8);
    logo.drop.setAttribute('transform', `translate(${fx(-(1 - p) * dist)} 0)`);
    logo.drop.style.opacity = fx(clamp((t - a) / 0.06, 0, 1), 3);
    logo.blur.setAttribute('stdDeviation', `${fx(Math.min(7, Math.abs(springVel(t - a, 1.6, 0.8) * dist) / 60 / 2.4))} 0`);
    const sp = seg(t, a, a + 0.75);
    logo.speed.style.opacity = t >= a ? fx((1 - ease.inCubic(sp)) * 0.9, 3) : '0';
    logo.speed.setAttribute('transform', `translate(${fx(-(1 - p) * dist - sp * 12)} 0)`);
    const lp = spring(t - 18.3, 2.4, 0.5);
    logo.leaf.setAttribute('transform', `translate(42.75 14.5) rotate(${fx((1 - lp) * -40)}) scale(${fx(Math.max(0.001, lp), 3)}) translate(-42.75 -14.5)`);
    logo.leaf.style.opacity = fx(clamp((t - 18.3) / 0.08, 0, 1), 3);
    const strength = [0.6, 0.3, 0.14];
    logo.echo.forEach((e, i) => {
      const s = 18.42 + i * 0.1;
      const q = ease.outCubic(seg(t, s, s + 0.4));
      e.setAttribute('transform', `translate(${fx((1 - q) * -7)} 0)`);
      e.style.strokeOpacity = fx(q * strength[i], 3);
    });
    logo.glow.style.opacity = fx(ease.outCubic(seg(t, 18.1, 18.8)) * (0.85 + 0.15 * Math.sin(t * 3)), 3);
    const w = ease.outExpo(seg(t, 18.3, 18.95));
    logo.word.style.visibility = t >= 18.3 ? '' : 'hidden';
    logo.word.style.clipPath = `inset(-12% ${fx((1 - w) * 100)}% -20% -4%)`;
    logo.word.style.transform = `translate3d(${fx((1 - w) * -30, 1)}px, 0, 0)`;
    logo.word.style.setProperty('--shine', fx(lerp(175, -75, ease.inOutSine(seg(t, 18.85, 19.75))), 1) + '%');
  }

  // ---------------------------------------------------------------- layout helpers
  // Headlines shrink to their column, so the layout holds with whichever font the machine has.
  function fitText() {
    document.querySelectorAll('.fit').forEach((el) => {
      el.style.fontSize = '';
      const box = el.parentElement;
      const cs = getComputedStyle(box);
      const max = box.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
      if (!(max > 0)) return;
      let size = parseFloat(getComputedStyle(el).fontSize);
      for (let i = 0; i < 6 && el.scrollWidth > max; i++) {
        size = Math.floor(size * (max / el.scrollWidth) * 0.99 * 10) / 10;
        el.style.fontSize = size + 'px';
      }
    });
  }
  function measure() {
    for (const k of kin) k.h = k.el.offsetHeight || 100;
    const cs = getComputedStyle(fg.scene);
    fg.cw = parseFloat(cs.getPropertyValue('--cw')) || 240;
    fg.slot = fg.cw + (VERTICAL ? 24 : 30);
    fg.w = fg.track.clientWidth;
    measureDoctor();
  }
  function fitStage() {
    if (RENDER) {
      stage.style.transform = '';
      return;
    }
    const s = Math.min(innerWidth / W, innerHeight / H);
    stage.style.transform = `translate(${fx((innerWidth - W * s) / 2, 1)}px, ${fx((innerHeight - H * s) / 2, 1)}px) scale(${fx(s, 5)})`;
  }

  // ---------------------------------------------------------------- frame
  const renderers = { 's-problem': renderProblem, 's-fix': renderFix, 's-pace': renderPace, 's-fg': renderFg, 's-doctor': renderDoctor, 's-pad': renderPad, 's-agents': renderAgents, 's-cta': renderCta };
  function draw(time) {
    const t = clamp(Number(time) || 0, 0, DURATION);
    const cam = camera(t);
    renderBackground(t, cam);
    renderParticles(t, cam);
    renderChrome(t);
    bg.world.style.transform = `translate3d(${fx(cam.x, 1)}px, ${fx(cam.y, 1)}px, 0) scale(${fx(cam.z, 4)})`;
    // Every renderer runs on every frame, hidden scenes included, so no state outlives its frame.
    for (const s of scenes) {
      s.el.style.visibility = t >= s.start && t <= s.end ? 'visible' : 'hidden';
      const key = Array.from(s.el.classList).find((c) => renderers[c]);
      if (key) renderers[key](t);
    }
    for (const k of kin) applyKin(k, t);
    if (bg.debug) bg.debug.textContent = `${t.toFixed(2)} s  ${sceneAt(t).el.className.replace('scene ', '')}`;
  }

  let manual = RENDER || STILL !== null;
  let paused = false;
  let clock = 0;
  let last = null;
  function tick(now) {
    if (manual) return;
    if (last !== null && !paused) clock += (now - last) / 1000;
    last = now;
    clock %= DURATION + 1.2;
    draw(Math.min(clock, DURATION));
    requestAnimationFrame(tick);
  }
  function startAutoplay() {
    manual = false;
    last = null;
    requestAnimationFrame(tick);
  }

  window.PROMO = { duration: DURATION, fps: FPS, format: FORMAT, width: W, height: H };
  window.renderAt = (t) => {
    manual = true;
    draw(t);
  };

  window.promoReady = (document.fonts && document.fonts.ready ? document.fonts.ready : Promise.resolve())
    .then(() => {
      buildParticles();
      buildGrain();
      buildFg();
      buildDoctor();
      buildTerm();
      setupGraphs();
      registerKinetics();
      fitText();
      measure();
      slamTimes = SLAMS();
      fitStage();
      addEventListener('resize', fitStage);
      const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
      if (STILL !== null) {
        draw(STILL);
      } else if (RENDER) {
        draw(0);
      } else if (reduce && !query.has('play')) {
        draw(19.6);
        const btn = $('play');
        btn.hidden = false;
        btn.addEventListener('click', () => {
          btn.hidden = true;
          clock = 0;
          startAutoplay();
        });
      } else {
        startAutoplay();
      }
      if (!RENDER) {
        addEventListener('keydown', (e) => {
          if (e.key === ' ') {
            paused = !paused;
            e.preventDefault();
          } else if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') {
            paused = true;
            const step = (e.shiftKey ? 1 : 1 / FPS) * (e.key === 'ArrowRight' ? 1 : -1);
            clock = clamp(clock + step, 0, DURATION);
            if (manual) draw(clock);
          }
        });
      }
      return true;
    });
})();
