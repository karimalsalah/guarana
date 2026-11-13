/* Guarana promo: 20 seconds, one clock, one canvas.
   renderAt(t) draws the whole frame for time t (seconds) from scratch: no state is kept between
   calls, so frames can be rendered in any order and come out identical. Motion blur is real: each
   output frame is the average of SUB sub-frames drawn inside the shutter window [t, t + SHUTTER/60).
   Timing comes from cues.json, which score.mjs reads too, so cuts and text land on the same hits.
   Query: ?format=vertical|landscape|square   ?w=&h= (any size)   ?mode=thumb (YouTube thumbnail)
          ?render=1 (exact size, no autoplay; render.mjs)   ?sub=<n> (sub-frames, default 4 when
          rendering and 1 when previewing)   ?t=<seconds> (one still)   ?debug=1 (time code)
   While previewing: space pauses, arrows step one frame (with shift, one second). The page is silent;
   the soundtrack is score.mjs. */
(function () {
  'use strict';

  const DUR = 20;
  const FPS = 60;
  const SIZES = { vertical: [1080, 1920], landscape: [1920, 1080], square: [1080, 1080] };
  const Q = new URLSearchParams(location.search);
  let W;
  let H;
  let FORMAT;
  if (Q.get('w') && Q.get('h')) {
    W = Math.round(Number(Q.get('w')));
    H = Math.round(Number(Q.get('h')));
    FORMAT = W > H * 1.2 ? 'landscape' : H > W * 1.2 ? 'vertical' : 'square';
  } else {
    FORMAT = SIZES[Q.get('format')] ? Q.get('format') : innerWidth > innerHeight ? 'landscape' : 'vertical';
    [W, H] = SIZES[FORMAT];
  }
  const THUMB = Q.get('mode') === 'thumb';
  const RENDER = Q.has('render');
  const SUB = Math.max(1, Math.round(Number(Q.get('sub') || (RENDER ? 4 : 1))));
  const SHUTTER = Number(Q.get('shutter') || 0.75);

  // ---------------------------------------------------------------- math
  const clamp = (x, a = 0, b = 1) => (x < a ? a : x > b ? b : x);
  const lerp = (a, b, p) => a + (b - a) * p;
  const sstep = (a, b, x) => { const p = clamp((x - a) / (b - a)); return p * p * (3 - 2 * p); };
  const eOut = (p) => 1 - Math.pow(1 - clamp(p), 3);
  const eOutE = (p) => (p >= 1 ? 1 : 1 - Math.pow(2, -10 * clamp(p)));
  const eIn = (p) => { p = clamp(p); return p * p * p; };
  const eIO = (p) => { p = clamp(p); return p < 0.5 ? 4 * p * p * p : 1 - Math.pow(-2 * p + 2, 3) / 2; };
  // Step response of a damped spring: 0 before dt = 0, settles at 1, overshoots.
  function spring(dt, freq, damping) {
    if (dt <= 0) return 0;
    const w = 2 * Math.PI * freq;
    const wd = w * Math.sqrt(1 - damping * damping);
    return 1 - Math.exp(-damping * w * dt) * (Math.cos(wd * dt) + ((damping * w) / wd) * Math.sin(wd * dt));
  }
  function hash01(n) {
    let x = (n | 0) ^ 0x9e3779b9;
    x = Math.imul(x ^ (x >>> 16), 0x85ebca6b);
    x = Math.imul(x ^ (x >>> 13), 0xc2b2ae35);
    x ^= x >>> 16;
    return (x >>> 0) / 4294967296;
  }
  const R = (i, s = 0) => hash01(i * 7919 + s * 104729 + 12345);
  // Smooth 1-D value noise in 0..1.
  function vn(x) {
    const i = Math.floor(x);
    const f = x - i;
    const s = f * f * (3 - 2 * f);
    return lerp(R(i, 77), R(i + 1, 77), s);
  }

  // ---------------------------------------------------------------- brand
  const AMBER = '255,201,74';
  const ORANGE = '242,133,26';
  const ECHO = '245,158,11';
  const HOT = '255,107,74';
  const GREEN = '60,207,126';
  const BLUE = '70,110,255';
  const WHITE = '255,255,255';
  const PAPER = '#F4F5F7';
  const MUTE = '#9AA6C0';
  const FF = '"Inter","Segoe UI Variable Display","Segoe UI",system-ui,-apple-system,"Helvetica Neue",Arial,"Liberation Sans",sans-serif';
  const MONO = '"JetBrains Mono","Cascadia Mono","Cascadia Code",Consolas,"DejaVu Sans Mono","Liberation Mono",monospace';

  // ---------------------------------------------------------------- canvases
  const mk = (w, h) => {
    const cv = document.createElement('canvas');
    cv.width = Math.max(1, Math.round(w));
    cv.height = Math.max(1, Math.round(h));
    return cv;
  };
  const out = document.getElementById('out');
  out.width = W;
  out.height = H;
  const octx = out.getContext('2d', { alpha: false });
  const scn = mk(W, H);
  const sctx = scn.getContext('2d', { alpha: false });
  const bgc = mk(W, H);
  const bctx = bgc.getContext('2d', { alpha: false });
  const acc = mk(W, H);
  const actx = acc.getContext('2d', { alpha: false });
  const tmp = mk(W, H);
  const tctx = tmp.getContext('2d', { alpha: false });
  const chn = mk(W, H);
  const chctx = chn.getContext('2d', { alpha: false });
  const b1 = mk(W / 4, H / 4);
  const b1c = b1.getContext('2d');
  const b2 = mk(W / 8, H / 8);
  const b2c = b2.getContext('2d');
  const b3 = mk(W / 16, H / 16);
  const b3c = b3.getContext('2d');
  const sk = mk(W / 8, H / 8);
  const skc = sk.getContext('2d');
  const mctx = mk(4, 4).getContext('2d');

  // ---------------------------------------------------------------- layout
  const u = Math.min(W, H) / 1080;
  const PORTRAIT = H > W * 1.2;
  const LAND = W > H * 1.2;
  const SQUARE = !PORTRAIT && !LAND;
  const safe = PORTRAIT
    ? { x0: 120 * u, x1: W - 120 * u, y0: 250 * u, y1: H - 480 * u }
    : LAND
      ? { x0: 150 * u, x1: W - 150 * u, y0: 90 * u, y1: H - 90 * u }
      : { x0: 90 * u, x1: W - 90 * u, y0: 90 * u, y1: H - 90 * u };
  safe.w = safe.x1 - safe.x0;
  safe.h = safe.y1 - safe.y0;
  const CX = W / 2;
  const CY = H / 2;
  const TOP = PORTRAIT ? safe.y0 : safe.y0 + 80 * u;
  const GRAPH = PORTRAIT
    ? { x0: safe.x0, x1: safe.x1, y0: safe.y1 - 440 * u, y1: safe.y1 - 10 * u }
    : { x0: safe.x0, x1: safe.x1, y0: safe.y1 - 300 * u, y1: safe.y1 };
  const AX = LAND ? safe.x0 : CX; // text anchor
  const AL = LAND ? 'left' : 'center';
  const rr = (c, x, y, w, h, r) => { c.beginPath(); c.roundRect(x, y, w, h, r); };

  // ---------------------------------------------------------------- cues
  let CUES = null;
  const EV = {};
  let SCENES = [];
  const KIND = {};
  function setCues(j) {
    CUES = j;
    SCENES = j.scenes.map((s) => ({ ...s }));
    for (const e of j.events) {
      EV[e.n] = e;
      (KIND[e.k] = KIND[e.k] || []).push(e);
    }
  }
  const sceneAt = (t) => {
    let f = SCENES[0];
    for (const s of SCENES) if (t >= s.start) f = s;
    return f;
  };
  const level = (t) => { for (const l of CUES.intensity) if (t >= l.from && t < l.to) return l.level; return 0.7; };

  // ---------------------------------------------------------------- text
  const widthCache = new Map();
  function tw(s, size, weight = 800, fam = FF) {
    const key = `${weight}|${fam}|${s}`;
    let w = widthCache.get(key);
    if (w === undefined) {
      mctx.font = `${weight} 100px ${fam}`;
      w = mctx.measureText(s).width;
      widthCache.set(key, w);
    }
    return (w * size) / 100;
  }
  const fit = (s, maxW, maxSize, weight = 800, fam = FF) => Math.min(maxSize, (maxW * 0.97) / tw(s, 1, weight, fam));
  let GRP = '';
  let FT = 0; // start of the frame being drawn: numbers use it so sub-frames do not blur digits together
  const G = (g) => { GRP = g; };
  const LOG = () => window.__layoutLog;

  // One line of text. o: size, weight, fam, fill (css color or 'ember'), align, alpha, sc, sx, sy, rot,
  // sweep (0..1 band position, adds a moving highlight), ls (tracking in px, drawn per character),
  // clip (draw inside a mask that rises with `rise` 0..1), grp, chk (false: not part of the layout check).
  function str(c, s, x, y, o) {
    const size = o.size;
    const weight = o.weight || 800;
    const fam = o.fam || FF;
    const alpha = o.alpha === undefined ? 1 : o.alpha;
    if (alpha <= 0.002 || size < 1) return 0;
    const align = o.align || 'center';
    const ls = o.ls || 0;
    const n = [...s].length;
    const width = tw(s, size, weight, fam) + ls * (n - 1);
    const sx = o.sx !== undefined ? o.sx : o.sc || 1;
    const sy = o.sy !== undefined ? o.sy : o.sc || 1;
    c.save();
    c.translate(x + (o.dx || 0), y + (o.dy || 0));
    if (o.rot) c.rotate(o.rot);
    c.scale(sx, sy);
    c.globalAlpha *= alpha;
    c.font = `${weight} ${size}px ${fam}`;
    c.textBaseline = 'middle';
    c.textAlign = 'left';
    const a0 = align === 'left' ? 0 : align === 'right' ? -width : -width / 2;
    const m = c.getTransform();
    if (LOG() && o.chk !== false && alpha >= 0.99 && Math.abs(m.a - 1) < 0.01 && Math.abs(m.d - 1) < 0.01) {
      const p1 = m.transformPoint({ x: a0, y: -0.46 * size });
      const p2 = m.transformPoint({ x: a0 + width, y: 0.46 * size });
      LOG().push({ s, grp: o.grp || GRP, x0: Math.min(p1.x, p2.x), y0: Math.min(p1.y, p2.y), x1: Math.max(p1.x, p2.x), y1: Math.max(p1.y, p2.y), size, free: !!o.free });
    }
    if (o.clip !== undefined) {
      c.beginPath();
      c.rect(a0 - size * 0.2, -size * 0.62, width + size * 0.4, size * 1.24);
      c.clip();
      c.translate(0, (1 - eOutE(o.clip)) * size * 1.15);
    }
    let fill = o.fill || PAPER;
    if (fill === 'ember') {
      const g = c.createLinearGradient(0, -size * 0.5, 0, size * 0.5);
      g.addColorStop(0, '#FFE08A');
      g.addColorStop(0.45, '#FFC94A');
      g.addColorStop(1, '#F2851A');
      fill = g;
    }
    const paint = () => {
      c.fillStyle = fill;
      if (!ls) c.fillText(s, a0, 0);
      else {
        let cx = a0;
        for (const ch of s) { c.fillText(ch, cx, 0); cx += tw(ch, size, weight, fam) + ls; }
      }
    };
    paint();
    if (o.sweep !== undefined && o.sweep > -0.3 && o.sweep < 1.3) {
      const p = o.sweep;
      const bw = 0.16;
      const g = c.createLinearGradient(a0, 0, a0 + width, 0);
      const stop = (v, col) => g.addColorStop(clamp(v), col);
      stop(p - bw, 'rgba(255,240,200,0)');
      stop(p, 'rgba(255,248,225,0.95)');
      stop(p + bw, 'rgba(255,240,200,0)');
      c.globalCompositeOperation = 'lighter';
      c.fillStyle = g;
      if (!ls) c.fillText(s, a0, 0);
      else { let cx = a0; for (const ch of s) { c.fillText(ch, cx, 0); cx += tw(ch, size, weight, fam) + ls; } }
    }
    c.restore();
    return width;
  }

  // Text made of coloured parts on one line.
  function mixed(c, parts, x, y, o) {
    const size = o.size;
    const weight = o.weight || 800;
    const fam = o.fam || FF;
    const total = parts.reduce((a, p) => a + tw(p.s, size, weight, fam), 0);
    const x0 = o.align === 'left' ? x : o.align === 'right' ? x - total : x - total / 2;
    c.save();
    c.translate(x0 + total / 2 + (o.dx || 0), y + (o.dy || 0));
    c.scale(o.sc || 1, o.sc || 1);
    c.globalAlpha *= o.alpha === undefined ? 1 : o.alpha;
    let cx = -total / 2;
    for (const p of parts) {
      const w = tw(p.s, size, weight, fam);
      str(c, p.s, cx, 0, { ...o, ...(p.o || {}), sc: 1, dx: 0, dy: 0, alpha: 1, fill: p.fill || o.fill, align: 'left' });
      cx += w;
    }
    c.restore();
    return total;
  }

  // ---------------------------------------------------------------- sprites
  const glowCache = {};
  function glowSpr(col) {
    if (glowCache[col]) return glowCache[col];
    const cv = mk(256, 256);
    const g = cv.getContext('2d');
    const rg = g.createRadialGradient(128, 128, 0, 128, 128, 128);
    rg.addColorStop(0, `rgba(${col},1)`);
    rg.addColorStop(0.12, `rgba(${col},0.62)`);
    rg.addColorStop(0.32, `rgba(${col},0.22)`);
    rg.addColorStop(0.62, `rgba(${col},0.05)`);
    rg.addColorStop(1, `rgba(${col},0)`);
    g.fillStyle = rg;
    g.fillRect(0, 0, 256, 256);
    glowCache[col] = cv;
    return cv;
  }
  function glow(c, x, y, r, col, a) {
    if (a <= 0.003 || r < 1) return;
    c.save();
    c.globalCompositeOperation = 'lighter';
    c.globalAlpha = Math.min(1, a);
    c.drawImage(glowSpr(col), x - r, y - r, r * 2, r * 2);
    c.restore();
  }
  const discCache = {};
  function discSpr(col) {
    if (discCache[col]) return discCache[col];
    const cv = mk(128, 128);
    const g = cv.getContext('2d');
    const rg = g.createRadialGradient(64, 64, 0, 64, 64, 64);
    rg.addColorStop(0, `rgba(${col},0.55)`);
    rg.addColorStop(0.7, `rgba(${col},0.42)`);
    rg.addColorStop(0.9, `rgba(${col},0.75)`);
    rg.addColorStop(1, `rgba(${col},0)`);
    g.fillStyle = rg;
    g.fillRect(0, 0, 128, 128);
    discCache[col] = cv;
    return cv;
  }
  const shaft = mk(128, 1024);
  {
    const g = shaft.getContext('2d');
    g.filter = 'blur(7px)';
    const lg = g.createLinearGradient(0, 0, 0, 1024);
    lg.addColorStop(0, 'rgba(255,224,170,0.95)');
    lg.addColorStop(0.3, 'rgba(255,190,100,0.38)');
    lg.addColorStop(1, 'rgba(255,150,60,0)');
    g.fillStyle = lg;
    g.beginPath();
    g.moveTo(60, 0);
    g.lineTo(68, 0);
    g.lineTo(122, 1024);
    g.lineTo(6, 1024);
    g.closePath();
    g.fill();
  }
  // The drop with its two speed-streak gaps, as in the logo mark (site/assets/logo-mark.svg).
  const DROP = new Path2D('M4 36L26.97 21.6A17 17 0 1 1 26.97 50.4Z');
  const dropSpr = mk(600, 600);
  {
    const g = dropSpr.getContext('2d');
    g.scale(12, 12);
    g.translate(0, -10);
    const lg = g.createLinearGradient(22, 19, 48, 53);
    lg.addColorStop(0, '#FFC94A');
    lg.addColorStop(1, '#F2851A');
    g.fillStyle = lg;
    g.fill(DROP);
    g.globalCompositeOperation = 'destination-out';
    g.lineWidth = 3.4;
    g.lineCap = 'round';
    g.beginPath();
    g.moveTo(-4, 29.2);
    g.lineTo(29, 29.2);
    g.moveTo(-4, 42.8);
    g.lineTo(29, 42.8);
    g.stroke();
  }
  const LEAF = new Path2D('M37.5 20Q38.5 10.5 48 9Q47.5 18.5 37.5 20Z');
  const ARCS = ['M53.93 22.98A17 17 0 0 1 53.93 49.02', 'M60.93 22.98A17 17 0 0 1 60.93 49.02', 'M67.93 22.98A17 17 0 0 1 67.93 49.02'].map((d) => new Path2D(d));
  // Logo mark centred on (x, y), `size` px wide for the drop-and-arcs art (about 66 units).
  function drawLogo(c, x, y, size, o = {}) {
    const s = size / 66;
    c.save();
    c.translate(x, y);
    c.scale(s, s);
    c.translate(-36, -31);
    c.globalAlpha *= o.alpha === undefined ? 1 : o.alpha;
    if (o.streaks) {
      c.save();
      c.lineCap = 'round';
      c.lineWidth = 3;
      const sp = o.streaks; // 0..1 how far the streaks have flown in
      const ln = (y0, from, to, a) => {
        c.strokeStyle = `rgba(${ECHO},${a})`;
        c.beginPath();
        c.moveTo(from + (to - from) * 0, y0);
        c.lineTo(to, y0);
        c.stroke();
      };
      const k = eOutE(sp);
      ln(29.2, -80 + k * 90 - 40, -80 + k * 90 + 20, 0.55 * (1 - sp * 0.4));
      ln(42.8, -90 + k * 100 - 40, -90 + k * 100 + 14, 0.55 * (1 - sp * 0.4));
      ln(36, -70 + k * 80 - 30, -70 + k * 80 + 12, 0.35 * (1 - sp * 0.5));
      c.restore();
    }
    c.save();
    c.translate(0, 10);
    c.globalAlpha *= o.drop === undefined ? 1 : o.drop;
    c.drawImage(dropSpr, 0, 0, 50, 50);
    c.restore();
    if (o.leaf !== 0) {
      const lp = o.leaf === undefined ? 1 : o.leaf;
      c.save();
      c.translate(37.5, 20);
      c.scale(lp, lp);
      c.translate(-37.5, -20);
      c.fillStyle = '#3CCF7E';
      c.fill(LEAF);
      c.restore();
    }
    c.lineCap = 'round';
    c.lineWidth = 3.2;
    for (let i = 0; i < 3; i++) {
      const a = o.arcs ? o.arcs[i] : 1;
      if (a <= 0) continue;
      c.strokeStyle = `rgba(${ECHO},${(0.9 - i * 0.22) * a})`;
      c.stroke(ARCS[i]);
    }
    c.restore();
  }

  // ---------------------------------------------------------------- effects
  function drawShafts(c, t, x, y, aimX, aimY, count, alpha, spread) {
    const aim = Math.atan2(aimX - x, aimY - y);
    const len = Math.hypot(W, H) * 1.15;
    c.save();
    c.globalCompositeOperation = 'lighter';
    c.translate(x, y);
    for (let i = 0; i < count; i++) {
      const f = (i + 0.5) / count;
      const off = lerp(-spread, spread, f) + 0.045 * Math.sin(t * 0.55 + i * 1.7) + (R(i, 5) - 0.5) * 0.12;
      const k = 0.5 + 1.6 * R(i, 6);
      const a = alpha * (0.55 + 0.45 * Math.sin(t * 0.8 + i * 2.3)) * (0.5 + R(i, 7));
      c.save();
      c.rotate(-(aim + off));
      c.globalAlpha = Math.min(1, a);
      c.drawImage(shaft, -len * 0.0625 * k, 0, len * 0.125 * k, len);
      c.restore();
    }
    c.restore();
  }
  function motes(c, t, count, seed, zMin, zMax, camX) {
    c.save();
    c.globalCompositeOperation = 'lighter';
    for (let i = 0; i < count; i++) {
      const z = lerp(zMin, zMax, R(i, seed + 1));
      const sp = (10 + 34 * z) * u;
      const px = R(i, seed + 2) * (W + 200 * u) - 100 * u + Math.sin(t * 0.35 + i) * 24 * z * u - camX * z;
      const py = (((R(i, seed + 3) * (H + 200 * u) - t * sp) % (H + 200 * u)) + H + 200 * u) % (H + 200 * u) - 100 * u;
      const tw = 0.55 + 0.45 * Math.sin(t * (1.3 + 2 * R(i, seed + 4)) + i * 3.1);
      const warm = R(i, seed + 5) < 0.7;
      if (z > 0.85) {
        const r = (16 + 34 * R(i, seed + 6)) * u * z;
        c.globalAlpha = 0.075 * tw;
        c.drawImage(discSpr(warm ? AMBER : WHITE), px - r, py - r, r * 2, r * 2);
      } else {
        const r = (1.1 + 3.2 * z) * u * (0.6 + R(i, seed + 6));
        c.globalAlpha = (0.18 + 0.55 * z) * tw;
        c.drawImage(glowSpr(warm ? AMBER : WHITE), px - r * 3, py - r * 3, r * 6, r * 6);
      }
    }
    c.restore();
  }
  function sparks(c, t, t0, x, y, n, speed, life, seed, col) {
    const dt = t - t0;
    if (dt < 0 || dt > life) return;
    c.save();
    c.globalCompositeOperation = 'lighter';
    c.lineCap = 'round';
    for (let i = 0; i < n; i++) {
      const a = R(i, seed) * Math.PI * 2;
      const v = speed * u * (0.25 + 0.95 * R(i, seed + 1));
      const k = 3.2;
      const d = (1 - Math.exp(-k * dt)) / k;
      const px = x + Math.cos(a) * v * d;
      const py = y + Math.sin(a) * v * d + 260 * u * dt * dt;
      const vel = v * Math.exp(-k * dt);
      const ln = Math.min(vel * 0.045, 90 * u);
      const fade = clamp(1 - dt / life);
      c.strokeStyle = `rgba(${col || AMBER},${0.95 * fade * fade})`;
      c.lineWidth = (1.2 + 2.4 * R(i, seed + 2)) * u;
      c.beginPath();
      c.moveTo(px, py);
      c.lineTo(px - Math.cos(a) * ln, py - Math.sin(a) * ln);
      c.stroke();
    }
    c.restore();
  }
  function ring(c, x, y, r, w, col, a) {
    if (a <= 0.003) return;
    c.save();
    c.globalCompositeOperation = 'lighter';
    c.strokeStyle = `rgba(${col},${a})`;
    c.lineWidth = w;
    c.beginPath();
    c.arc(x, y, Math.max(0.1, r), 0, Math.PI * 2);
    c.stroke();
    c.restore();
  }
  function floorGrid(c, t, alpha, horizonY, scroll, tint) {
    if (alpha <= 0.003) return;
    c.save();
    c.beginPath();
    c.rect(0, horizonY, W, H - horizonY);
    c.clip();
    c.globalCompositeOperation = 'lighter';
    c.lineWidth = 1.6 * u;
    const depth = H - horizonY;
    for (let k = 0; k < 18; k++) {
      const d = (((k + scroll) % 18) + 18) % 18 / 18; // 0 far .. 1 near
      const y = horizonY + depth * Math.pow(d, 2.4);
      c.strokeStyle = `rgba(${tint},${alpha * (0.2 + 0.8 * d)})`;
      c.beginPath();
      c.moveTo(0, y);
      c.lineTo(W, y);
      c.stroke();
    }
    for (let i = -14; i <= 14; i++) {
      const xb = CX + i * W * 0.2;
      const g = c.createLinearGradient(0, horizonY, 0, H);
      g.addColorStop(0, `rgba(${tint},0)`);
      g.addColorStop(1, `rgba(${tint},${alpha * 0.9})`);
      c.strokeStyle = g;
      c.beginPath();
      c.moveTo(CX + i * W * 0.012, horizonY);
      c.lineTo(xb, H);
      c.stroke();
    }
    c.restore();
  }

  // ---------------------------------------------------------------- scene styles and camera
  const STYLE = {
    open: { dolly: 0.1, glow: 0, floor: 0, shafts: 0, light: [0.5, 0.5] },
    problem: { dolly: 0.05, glow: 0.55, floor: 0.32, shafts: 3, light: [0.12, -0.08], hot: 1, pre: [0.3, 0.14] },
    fix: { dolly: 0.1, glow: 0.95, floor: 0.6, shafts: 6, light: [0.88, -0.08], pre: [0.3, 0.14] },
    devices: { dolly: 0.12, glow: 0.9, floor: 0.7, shafts: 7, light: [0.5, -0.1], pre: [0.8, 1.1] },
    plug: { dolly: 0.06, glow: 0.8, floor: 0.45, shafts: 5, light: [0.1, -0.06], pre: [0.3, 0.14] },
    ai: { dolly: 0.08, glow: 0.85, floor: 0.55, shafts: 6, light: [0.92, 0.02], pre: [0.3, 0.14] },
    legacy: { dolly: 0.08, glow: 1, floor: 0.5, shafts: 8, light: [0.5, -0.12], pre: [0.5, 0.5] },
    cta: { dolly: 0.04, glow: 1.15, floor: 0.4, shafts: 11, light: [0.5, 0.38] },
  };
  const NOCAM = () => !!window.__nocam;
  function camera(t, S) {
    if (NOCAM()) return { z: 1, x: 0, y: 0, r: 0 };
    const st = STYLE[S.id];
    const lt = t - S.start;
    const dur = S.end - S.start;
    // The problem scene's camera drifts at 14.8 fps: it stutters, the rest of the film does not.
    const tq = S.id === 'problem' ? Math.floor(t * 14.8) / 14.8 : t;
    let z = 1 + st.dolly * clamp((tq - S.start) / dur);
    const toEnd = S.end - t;
    if (S.end < DUR - 0.01 && st.pre && toEnd < st.pre[0]) z += st.pre[1] * eIn(1 - toEnd / st.pre[0]);
    if (S.start > 0) z += 0.13 * Math.exp(-lt / 0.15);
    let x = Math.sin(tq * 0.45 + S.start) * 10 * u;
    let y = Math.cos(tq * 0.37 + S.start) * 7 * u;
    let r = Math.sin(tq * 0.3 + S.start * 2) * 0.004;
    for (const k of ['boom', 'hit', 'glitch']) {
      const amp = k === 'boom' ? 20 : k === 'hit' ? 7 : 12;
      for (const e of KIND[k] || []) {
        const dt = t - e.t;
        if (dt >= 0 && dt < 0.6) {
          const a = amp * u * e.v * Math.exp(-dt / 0.09);
          x += a * (vn(t * 41 + e.t * 9) * 2 - 1);
          y += a * (vn(t * 37 + e.t * 5 + 40) * 2 - 1) * 0.8;
          r += (a / 1500) * (vn(t * 29 + e.t * 3) * 2 - 1);
        }
      }
    }
    return { z, x, y, r };
  }
  function applyCam(c, cam, k = 1) {
    c.translate(CX + cam.x * k, CY + cam.y * k);
    c.rotate(cam.r * k);
    const z = 1 + (cam.z - 1) * k;
    c.scale(z, z);
    c.translate(-CX, -CY);
  }
  const glitchAmt = (t) => {
    let g = 0;
    for (const e of KIND.glitch || []) { const dt = t - e.t; if (dt >= 0) g += e.v * Math.exp(-dt / 0.12); }
    return Math.min(1.4, g);
  };

  // ---------------------------------------------------------------- background
  function drawBg(c, t, S) {
    const st = STYLE[S.id];
    const cam = camera(t, S);
    const lv = level(t);
    const hot = st.hot || 0;
    const grd = c.createLinearGradient(0, 0, 0, H);
    grd.addColorStop(0, '#05070f');
    grd.addColorStop(0.55, hot ? '#0c0d1c' : '#091128');
    grd.addColorStop(1, hot ? '#14101c' : '#0d1a3c');
    c.globalCompositeOperation = 'source-over';
    c.fillStyle = grd;
    c.fillRect(0, 0, W, H);
    if (S.id === 'open') return;
    c.save();
    applyCam(c, cam, 0.5);
    const lx = st.light[0] * W;
    const ly = st.light[1] * H;
    const big = Math.max(W, H);
    const pulse = 0.82 + 0.18 * Math.sin(t * Math.PI);
    glow(c, lx, ly, big * 0.95, hot ? HOT : ORANGE, 0.34 * st.glow * pulse);
    glow(c, lx, ly, big * 0.5, AMBER, 0.22 * st.glow * pulse);
    glow(c, W - lx, H * 0.95, big * 0.8, BLUE, 0.2);
    if (!hot) glow(c, CX, H * 1.02, big * 0.7, '40,70,200', 0.2);
    floorGrid(c, t, 0.15 * st.floor * (0.6 + 0.4 * lv), H * (PORTRAIT ? 0.66 : 0.64), t * 0.9 * lv, hot ? HOT : AMBER);
    drawShafts(c, t, lx, ly, CX, CY, st.shafts, 0.09 * st.glow * (0.6 + 0.4 * lv), 0.5);
    motes(c, t, Math.round(70 * (W * H) / (1080 * 1920) + 40), 0, 0.2, 0.62, cam.x);
    c.restore();
  }

  // ---------------------------------------------------------------- animation helpers
  const slamSc = (dt) => (dt < 0 ? 1 : lerp(1.75, 1, spring(dt, 2.6, 0.5)));
  const popA = (dt, d = 0.06) => clamp(dt / d);
  const exitP = (t, t1, d = 0.14) => clamp((t - (t1 - d)) / d);

  // Kinetic headline line: slam / rise / fade-up from `t0`, leaving at `t1` (optional).
  function kin(c, s, x, y, size, t, t0, o = {}) {
    const dt = t - t0;
    if (dt < 0) return 0;
    const mode = o.mode || 'slam';
    const ex = o.t1 ? exitP(t, o.t1) : 0;
    const base = { size, align: o.align || AL, fill: o.fill, weight: o.weight, fam: o.fam, grp: o.grp, chk: o.chk, free: o.free, ls: o.ls };
    let a = 1 - ex;
    let sc = 1;
    let dy = -28 * u * ex;
    if (mode === 'slam') { sc = slamSc(dt); a *= popA(dt); } else if (mode === 'fade') { a *= eOut(dt / 0.45); dy += (1 - eOut(dt / 0.45)) * 24 * u; } else if (mode === 'rise') { base.clip = clamp(dt / 0.5); a *= popA(dt, 0.04); }
    const sweepT = o.sweepAt !== undefined ? (t - o.sweepAt) / 0.7 : undefined;
    return str(c, s, x, y + dy, { ...base, alpha: a * (o.alpha === undefined ? 1 : o.alpha), sc, sx: o.sx !== undefined ? o.sx * sc : sc, sweep: sweepT !== undefined && sweepT > -0.3 && sweepT < 1.3 ? sweepT : undefined });
  }

  // ---------------------------------------------------------------- frame-time graph
  const SPIKES = [[2.12, 0.55], [2.31, 0.8], [2.5, 1], [2.62, 0.5], [2.75, 1], [2.93, 0.7], [3.125, 1], [3.3, 0.6], [3.5, 1], [3.68, 0.7], [3.85, 0.9], [4.1, 0.8], [4.4, 0.7], [4.8, 0.8], [5.2, 0.6], [5.6, 0.5], [6.0, 0.5], [6.4, 0.4]];
  const AMPK = [[0, 1], [4.7, 1], [5.4, 0.55], [6.1, 0.5], [6.9, 0]];
  function ampAt(tau) {
    if (tau <= AMPK[0][0]) return 1;
    for (let i = 0; i < AMPK.length - 1; i++) {
      if (tau <= AMPK[i + 1][0]) return lerp(AMPK[i][1], AMPK[i + 1][1], sstep(AMPK[i][0], AMPK[i + 1][0], tau));
    }
    return 0;
  }
  // Illustrative frame time in ms at time tau: spiky, then settling onto the 16.67 ms rail.
  function msAt(tau) {
    const a = ampAt(tau);
    if (a <= 0) return 16.67;
    let s = 0;
    for (const [ts, k] of SPIKES) {
      const d = tau - ts;
      if (d > -0.2 && d < 0.4) s += k * 21 * Math.exp(-Math.pow(d < 0 ? d / 0.022 : d / 0.06, 2));
    }
    const v = 5 + 8 * vn(tau * 6.5) + 3 * vn(tau * 19);
    return 16.67 + a * Math.min(v + s, 35.5);
  }
  function drawGraph(c, t, alpha = 1) {
    const g = GRAPH;
    const gw = g.x1 - g.x0;
    const gh = g.y1 - g.y0;
    const t0 = EV.cut_problem.t;
    const pin = clamp((t - t0) / 0.3);
    if (pin <= 0) return;
    const lockT = EV.locked.t;
    const lockP = clamp((t - lockT) / 0.35);
    c.save();
    c.globalAlpha *= eOut(pin) * alpha;
    rr(c, g.x0, g.y0, gw, gh, 24 * u);
    c.fillStyle = 'rgba(7,12,30,0.62)';
    c.fill();
    c.strokeStyle = `rgba(${AMBER},${0.14 + 0.3 * lockP})`;
    c.lineWidth = 2 * u;
    c.stroke();
    c.save();
    rr(c, g.x0, g.y0, gw, gh, 24 * u);
    c.clip();
    const top = g.y0 + 62 * u;
    const bot = g.y1 - 56 * u;
    const y = (ms) => bot - (ms / 60) * (bot - top);
    // reference lines
    c.lineWidth = 1.5 * u;
    c.setLineDash([8 * u, 10 * u]);
    c.strokeStyle = 'rgba(160,175,215,0.32)';
    c.beginPath();
    c.moveTo(g.x0, y(50));
    c.lineTo(g.x1, y(50));
    c.stroke();
    c.setLineDash([]);
    // the 16.67 ms rail
    const yr = y(16.67);
    c.strokeStyle = `rgba(${AMBER},${0.3 + 0.55 * lockP})`;
    c.lineWidth = (2 + 3 * lockP) * u;
    c.beginPath();
    c.moveTo(g.x0, yr);
    c.lineTo(g.x1, yr);
    c.stroke();
    if (lockP > 0) glow(c, CX, yr, gw * 0.5, AMBER, 0.0);
    // the trace
    const win = 3.0;
    const x1 = g.x1 - 22 * u;
    const pts = [];
    const step = 1 / 80;
    for (let tau = Math.max(t0, t - win); tau <= t + 1e-6; tau += step) {
      pts.push([x1 - ((t - tau) / win) * (x1 - g.x0), y(msAt(tau))]);
    }
    if (pts.length > 1) {
      const lg = c.createLinearGradient(0, y(52), 0, yr);
      lg.addColorStop(0, `rgb(255,${Math.round(lerp(90, 110, lockP))},60)`);
      lg.addColorStop(0.5, '#FF9A3C');
      lg.addColorStop(1, '#FFC94A');
      const path = new Path2D();
      pts.forEach((p, i) => (i ? path.lineTo(p[0], p[1]) : path.moveTo(p[0], p[1])));
      c.lineJoin = 'round';
      c.lineCap = 'round';
      c.globalCompositeOperation = 'lighter';
      c.strokeStyle = lg;
      c.globalAlpha *= 0.28;
      c.lineWidth = 18 * u;
      c.stroke(path);
      c.globalAlpha /= 0.28;
      c.globalCompositeOperation = 'source-over';
      c.lineWidth = 5.5 * u;
      c.stroke(path);
    }
    c.restore();
    const hy = y(msAt(t));
    const hx = x1;
    glow(c, hx, hy, 46 * u, lockP > 0.5 ? AMBER : HOT, 0.85);
    c.fillStyle = '#fff';
    c.beginPath();
    c.arc(hx, hy, 7 * u, 0, Math.PI * 2);
    c.fill();
    if (lockP > 0) ring(c, hx, hy, 14 * u + (t - lockT) * 260 * u, 4 * u, AMBER, 0.8 * (1 - lockP));
    // labels
    str(c, '50 ms', g.x0 + 24 * u, g.y0 + 28 * u, { size: 25 * u, weight: 600, fam: MONO, fill: '#8F9BBB', align: 'left', grp: 'graph' });
    str(c, '16.7 ms', g.x1 - 24 * u, yr + 28 * u, { size: 26 * u, weight: 700, fam: MONO, fill: lockP > 0.2 ? '#FFD36B' : '#C9A24A', align: 'right', grp: 'graph' });
    str(c, 'frame time, illustrative', g.x0 + 24 * u, g.y1 - 26 * u, { size: 23 * u, weight: 500, fam: MONO, fill: '#7C88A8', align: 'left', grp: 'graph' });
    c.restore();
  }

  // ---------------------------------------------------------------- scene: cold open
  function sOpen(c, t) {
    G('open');
    let pulse = 0;
    for (const e of KIND.heart) { const d = t - e.t; if (d >= 0) pulse += e.v * Math.exp(-d / 0.2); }
    glow(c, CX, CY, Math.max(W, H) * 0.6, AMBER, 0.09 * Math.min(pulse, 1.5) * (0.6 + 0.4 * eIn(t / 2)));
    glow(c, CX, CY, Math.max(W, H) * 0.3, ORANGE, 0.12 * Math.min(pulse, 1.5));
    // ECG trace: the heartbeat becomes the frame-time line
    const yE = CY + 300 * u;
    const xa = safe.x0;
    const xb = safe.x1;
    const head = xa + (xb - xa) * clamp(t / 2);
    const blip = (dx, v) => v * 120 * u * (Math.exp(-Math.pow(dx / (6 * u), 2)) - 0.32 * Math.exp(-Math.pow((dx - 16 * u) / (9 * u), 2)) + 0.1 * Math.exp(-Math.pow((dx + 24 * u) / (11 * u), 2)));
    const path = new Path2D();
    let hy = yE;
    for (let x = xa; x <= head + 0.01; x += 3 * u) {
      let y = yE;
      for (const e of KIND.heart) y -= blip(x - (xa + ((xb - xa) * e.t) / 2), e.v);
      hy = y;
      x === xa ? path.moveTo(x, y) : path.lineTo(x, y);
    }
    c.save();
    c.lineJoin = 'round';
    c.lineCap = 'round';
    const lg = c.createLinearGradient(xa, 0, Math.max(head, xa + 1), 0);
    lg.addColorStop(0, `rgba(${AMBER},0)`);
    lg.addColorStop(1, `rgba(${AMBER},1)`);
    c.strokeStyle = lg;
    c.globalCompositeOperation = 'lighter';
    c.globalAlpha = 0.3;
    c.lineWidth = 16 * u;
    c.stroke(path);
    c.globalAlpha = 1;
    c.lineWidth = 4.5 * u;
    c.stroke(path);
    c.restore();
    if (t > 0.02) glow(c, head, hy, 40 * u, AMBER, 0.9);
    const maxW = Math.min(safe.w, 1200 * u);
    const s1 = fit('Ocarina of Time.', maxW, 92 * u, 700);
    const s2 = fit('On a laptop.', maxW, 170 * u, 900);
    const p1 = clamp((t - EV.ocarina.t) / 0.8);
    const fade = 1 - sstep(1.9, 1.99, t);
    if (t >= EV.ocarina.t) str(c, 'Ocarina of Time.', CX, CY - 110 * u, { size: s1, weight: 700, fill: '#E6EAF5', alpha: eOut(p1) * 0.95, ls: (1 - eOut(p1)) * 22 * u + 2 * u, grp: 'open1' });
    kin(c, 'On a laptop.', CX, CY + 30 * u, s2, t, EV.laptop.t, { align: 'center', fill: 'ember', weight: 900, sweepAt: EV.laptop.t + 0.15, grp: 'open2' });
    c.fillStyle = `rgba(0,0,0,${1 - fade})`;
    c.fillRect(0, 0, W, H);
  }

  // ---------------------------------------------------------------- scene: the problem
  function sProblem(c, t) {
    G('problem');
    const R0 = TOP;
    const R1 = GRAPH.y0 - 30 * u;
    const h = R1 - R0;
    const g = glitchAmt(t);
    const jx = (vn(t * 47) - 0.5) * 44 * u * Math.min(g, 1);
    drawGraph(c, t);
    const maxW = LAND ? safe.w * 0.8 : safe.w;
    const hs = fit('14.8 fps.', maxW, 330 * u, 900);
    const ss = fit('50 ms stutters.', maxW, 128 * u, 800);
    const y1 = R0 + h * 0.34;
    const y2 = R0 + h * 0.8;
    const t1 = EV.fps148.t;
    if (t >= t1) {
      const dt = t - t1;
      const a = popA(dt);
      const sc = slamSc(dt);
      // chromatic ghosts while the frame time spikes
      if (g > 0.05) {
        for (const [col, d] of [['255,60,90', -1], ['60,200,255', 1]]) {
          c.save();
          c.globalCompositeOperation = 'lighter';
          str(c, '14.8 fps.', AX + jx + d * g * 12 * u, y1, { size: hs, weight: 900, fill: `rgba(${col},0.5)`, align: AL, sc, alpha: a * 0.6, chk: false });
          c.restore();
        }
      }
      str(c, '14.8 fps.', AX + jx, y1, { size: hs, weight: 900, fill: `rgb(${HOT})`, align: AL, sc, alpha: a, grp: 'p1', chk: g < 0.05 });
    }
    const t2 = EV.stutters.t;
    if (t >= t2) {
      const dt = t - t2;
      mixed(c, [{ s: '50 ms', fill: `rgb(${HOT})` }, { s: ' stutters.', fill: PAPER }], AX + jx * 0.6, y2, { size: ss, weight: 800, align: AL, sc: slamSc(dt), alpha: popA(dt), grp: 'p2', chk: g < 0.05 });
    }
  }

  // ---------------------------------------------------------------- scene: the fix
  function arrow(c, x0, x1, y, col, a, w) {
    c.save();
    c.strokeStyle = `rgba(${col},${a})`;
    c.lineWidth = w;
    c.lineCap = 'round';
    c.lineJoin = 'round';
    c.beginPath();
    c.moveTo(x0, y);
    c.lineTo(x1, y);
    c.moveTo(x1 - 14 * u, y - 12 * u);
    c.lineTo(x1, y);
    c.lineTo(x1 - 14 * u, y + 12 * u);
    c.stroke();
    c.restore();
  }
  function sFix(c, t) {
    G('fix');
    const R0 = TOP;
    const R1 = GRAPH.y0 - 30 * u;
    const h = R1 - R0;
    drawGraph(c, t);
    const tA = EV.fixhead.t;
    const tB = EV.framegen.t;
    const tC = EV.locked.t;
    const textW = LAND ? safe.w * 0.54 : safe.w;
    const vis = LAND
      ? { x0: safe.x0 + safe.w * 0.6, x1: safe.x1, y0: R0 + 40 * u, y1: R1 }
      : { x0: safe.x0, x1: safe.x1, y0: R0 + (SQUARE ? 300 : 455) * u, y1: R1 };
    const kS = (PORTRAIT ? 64 : SQUARE ? 54 : 62) * u;
    if (t < tB) {
      const ex = exitP(t, tB);
      // counter 14.8 -> 22.8, landing on the beat
      const cp = clamp((FT - tA) / (EV.fps228.t - tA));
      const val = lerp(14.8, 22.8, eIO(cp));
      const landed = t >= EV.fps228.t;
      const numStr = (landed ? 22.8 : Math.round(val * 10) / 10).toFixed(1);
      const wt = (s) => tw('22.8', s, 900) + 0.12 * s + tw('fps', 0.4 * s, 800);
      const hs = Math.min((PORTRAIT ? 270 : SQUARE ? 215 : 280) * u, textW / (wt(1)));
      const yK = R0 + kS * 0.62;
      const yH = R0 + kS * 1.25 + hs * 0.46;
      kin(c, 'one shader fix:', AX, yK, kS, t, tA, { t1: tB, fill: PAPER, weight: 700, grp: 'fk' });
      const lp = landed ? spring(t - EV.fps228.t, 3, 0.5) : 0;
      const sc = landed ? lerp(1.18, 1, clamp(lp, 0, 1.4)) : 1;
      const dtN = t - tA;
      const ia = popA(dtN) * (1 - ex);
      const wN = tw('22.8', hs, 900) + 0.12 * hs + tw('fps', 0.4 * hs, 800);
      const x0 = LAND ? AX : AX - wN / 2;
      c.save();
      c.translate(0, -28 * u * ex);
      c.translate(x0 + wN / 2, yH);
      c.scale(sc, sc);
      c.translate(-(x0 + wN / 2), -yH);
      // fixed-width digits so the counter does not wobble
      const cell = tw('0', hs, 900);
      let cx = x0;
      for (const ch of numStr) {
        const w = ch === '.' ? tw('.', hs, 900) : cell;
        str(c, ch, cx + w / 2, yH, { size: hs, weight: 900, fill: 'ember', alpha: ia, sc: 1, grp: 'fnum', chk: false });
        cx += w;
      }
      const numEnd = x0 + 3 * cell + tw('.', hs, 900);
      str(c, 'fps', numEnd + 0.12 * hs, yH + hs * 0.1, { size: 0.4 * hs, weight: 800, fill: PAPER, align: 'left', alpha: ia, grp: 'ffps' });
      if (LOG() && ia >= 0.99) LOG().push({ s: numStr + ' fps', grp: 'fnum', x0: x0, x1: numEnd, y0: yH - hs * 0.4, y1: yH + hs * 0.4, size: hs });
      if (landed) {
        const dl = t - EV.fps228.t;
        ring(c, x0 + wN / 2, yH, 60 * u + dl * 700 * u, 6 * u, AMBER, 0.8 * Math.max(0, 1 - dl / 0.3));
      }
      c.restore();
      // was 14.8
      if (landed && LAND) {
        const wa = popA(t - EV.fps228.t, 0.2) * (1 - ex);
        str(c, 'was 14.8 fps', LAND ? x0 : CX, yH + hs * 0.62, { size: 30 * u, weight: 600, fam: MONO, fill: '#8C93A0', align: LAND ? 'left' : 'center', alpha: wa, grp: 'fwas' });
      }
      // foliage draw
      const bT = EV.fps228.t;
      if (t >= bT) {
        const ba = popA(t - bT, 0.18) * (1 - ex);
        const bw = vis.x1 - vis.x0;
        const yb = LAND ? vis.y0 + 60 * u : vis.y0 + 10 * u;
        const bp = clamp((FT - bT - 0.15) / 0.7);
        const ms = lerp(15.7, 5.8, eOutE(bp));
        c.save();
        c.translate(0, -28 * u * ex);
        c.globalAlpha *= ba;
        str(c, 'Hyrule Field, foliage draw', vis.x0, yb, { size: 28 * u, weight: 600, fam: MONO, fill: '#B9C3DD', align: 'left', grp: 'fb' });
        const by = yb + 46 * u;
        const bh = 50 * u;
        rr(c, vis.x0, by, bw, bh, bh / 2);
        c.fillStyle = 'rgba(30,42,78,0.7)';
        c.fill();
        const fw = Math.max(bh, (bw * ms) / 15.7);
        rr(c, vis.x0, by, fw, bh, bh / 2);
        const lg = c.createLinearGradient(vis.x0, 0, vis.x0 + bw, 0);
        lg.addColorStop(0, '#FFC94A');
        lg.addColorStop(1, '#F2851A');
        c.fillStyle = lg;
        c.fill();
        glow(c, vis.x0 + fw, by + bh / 2, 60 * u, AMBER, 0.5);
        const yv = by + bh + 54 * u;
        str(c, '15.7 ms', vis.x0, yv, { size: 38 * u, weight: 700, fam: MONO, fill: '#8C93A0', align: 'left', grp: 'fb' });
        arrow(c, vis.x0 + 180 * u, vis.x0 + 270 * u, yv, AMBER, 0.85, 4 * u);
        str(c, `${ms.toFixed(1)} ms`, vis.x1, yv, { size: 84 * u, weight: 900, fill: 'ember', align: 'right', grp: 'fb2' });
        c.restore();
      }
    } else if (t < tC) {
      const ex = exitP(t, tC);
      kin(c, 'frame generation', AX, R0 + kS * 0.62, kS, t, tB, { t1: tC, fill: PAPER, weight: 700, grp: 'gk' });
      const hs = fit('that predicts.', textW, (PORTRAIT ? 190 : SQUARE ? 170 : 190) * u, 900);
      kin(c, 'that predicts.', AX, R0 + kS * 1.25 + hs * 0.46, hs, t, tB + 0.08, { t1: tC, fill: 'ember', weight: 900, sweepAt: tB + 0.3, grp: 'gh' });
      // film strip: real, predicted, real, predicted...
      const sx0 = safe.x0;
      const sx1 = safe.x1;
      const fw = (sx1 - sx0) * (LAND ? 0.13 : 0.2);
      const fh = fw * 0.62;
      const gap = fw * 0.14;
      const pitch = fw + gap;
      const sy = LAND ? R0 + 300 * u : vis.y0 + 30 * u;
      const ea = popA(t - tB - 0.1, 0.25) * (1 - ex);
      c.save();
      c.globalAlpha *= ea;
      c.translate(0, -28 * u * ex);
      rr(c, sx0 - 10 * u, sy - 30 * u, sx1 - sx0 + 20 * u, fh + 60 * u, 18 * u);
      c.fillStyle = 'rgba(8,14,34,0.55)';
      c.fill();
      c.save();
      c.beginPath();
      c.rect(sx0, sy - 40 * u, sx1 - sx0, fh + 80 * u);
      c.clip();
      const scroll = (t - tB) * (pitch / 0.25);
      for (let i = -1; i < 10; i++) {
        const k = i + Math.floor(scroll / pitch);
        const x = sx0 + 8 * u + i * pitch - (scroll % pitch);
        const pred = ((k % 2) + 2) % 2 === 1;
        rr(c, x, sy, fw, fh, 10 * u);
        if (pred) {
          c.fillStyle = 'rgba(245,158,11,0.10)';
          c.fill();
          c.setLineDash([9 * u, 7 * u]);
          c.strokeStyle = `rgba(${ECHO},0.9)`;
        } else {
          const lg = c.createLinearGradient(x, sy, x + fw, sy + fh);
          lg.addColorStop(0, 'rgba(255,201,74,0.34)');
          lg.addColorStop(1, 'rgba(242,133,26,0.18)');
          c.fillStyle = lg;
          c.fill();
          c.strokeStyle = `rgba(${AMBER},0.95)`;
        }
        c.lineWidth = 3 * u;
        c.stroke();
        c.setLineDash([]);
        // a ball crossing the frames: the predicted frame sits halfway between the real ones
        const bx = x + fw * (0.15 + 0.7 * (((k * 0.5) % 1.6 + 1.6) % 1.6) / 1.6);
        glow(c, bx, sy + fh * 0.55, fh * 0.45, pred ? ECHO : AMBER, pred ? 0.55 : 0.9);
        c.fillStyle = pred ? `rgba(${ECHO},0.95)` : '#FFE9A8';
        c.beginPath();
        c.arc(bx, sy + fh * 0.55, fh * 0.1, 0, Math.PI * 2);
        c.fill();
      }
      c.restore();
      // legend
      const ly = sy + fh + 62 * u;
      rr(c, sx0 + 4 * u, ly - 12 * u, 36 * u, 24 * u, 6 * u);
      c.fillStyle = 'rgba(255,201,74,0.5)';
      c.fill();
      str(c, 'real frame', sx0 + 54 * u, ly, { size: 27 * u, weight: 600, fam: MONO, fill: '#D3DAEC', align: 'left', grp: 'leg1' });
      const lx2 = sx0 + (sx1 - sx0) * 0.5;
      rr(c, lx2, ly - 12 * u, 36 * u, 24 * u, 6 * u);
      c.setLineDash([6 * u, 5 * u]);
      c.strokeStyle = `rgba(${ECHO},0.95)`;
      c.lineWidth = 3 * u;
      c.stroke();
      c.setLineDash([]);
      str(c, 'predicted', lx2 + 50 * u, ly, { size: 27 * u, weight: 600, fam: MONO, fill: '#D3DAEC', align: 'left', grp: 'leg2' });
      c.restore();
    } else {
      const dt = t - tC;
      const hs = fit('16.7 ms.', textW, (PORTRAIT ? 270 : SQUARE ? 220 : 250) * u, 900);
      const ls = fit('Locked.', textW, (PORTRAIT ? 200 : SQUARE ? 170 : 180) * u, 900);
      const y1 = R0 + hs * 0.5;
      const y2 = y1 + hs * 0.5 + ls * 0.46;
      kin(c, '16.7 ms.', AX, y1, hs, t, tC, { fill: 'ember', weight: 900, sweepAt: tC + 0.1, grp: 'lh' });
      kin(c, 'Locked.', AX, y2, ls, t, tC + 0.1, { fill: PAPER, weight: 900, sweepAt: tC + 0.35, grp: 'ls' });
      const chipA = popA(dt - 0.35, 0.2);
      const chips = ['p50 16.7 ms', 'title screen: 100% of frames within 2 ms'];
      let cy = LAND ? R0 + 150 * u : y2 + ls * 0.46 + 60 * u;
      let cxL = LAND ? vis.x0 : AX;
      const cs = Math.min(31 * u, ((LAND ? safe.x1 - vis.x0 : safe.w) - 50 * u) / tw(chips[1], 1, 600, MONO));
      for (let i = 0; i < 2; i++) {
        const w = tw(chips[i], cs, 600, MONO) + 44 * u;
        const px = LAND ? cxL : AX - w / 2;
        c.save();
        c.globalAlpha *= popA(dt - 0.35 - i * 0.12, 0.2);
        rr(c, px, cy - cs * 0.95, w, cs * 1.9, cs * 0.95);
        c.fillStyle = 'rgba(255,201,74,0.1)';
        c.fill();
        c.strokeStyle = `rgba(${AMBER},0.6)`;
        c.lineWidth = 2 * u;
        c.stroke();
        str(c, chips[i], px + w / 2, cy, { size: cs, weight: 600, fam: MONO, fill: '#FFE3A0', grp: `lc${i}` });
        c.restore();
        cy += cs * 2.5;
      }
      void chipA;
      // title-screen flash lines either side of the rail
      const yr = GRAPH.y0 + 62 * u + (1 - 16.67 / 60) * (GRAPH.y1 - 56 * u - GRAPH.y0 - 62 * u);
      void yr;
    }
  }

  // ---------------------------------------------------------------- scene: every device
  const CHIPS = ['Intel Arc 140T', 'Intel Arc 140V', 'Meteor Lake', 'Panther Lake', 'Radeon 780M', 'Radeon 890M', 'Radeon 8060S', 'ROG Ally X', 'Legion Go 2', 'MSI Claw', 'Steam Deck', 'NVIDIA desktop'];
  function sDevices(c, t) {
    G('devices');
    const kS = (PORTRAIT ? 66 : SQUARE ? 56 : 70) * u;
    let box;
    let cols;
    let rows;
    let chipH;
    if (LAND) { box = { x0: safe.x0 + safe.w * 0.52, x1: safe.x1, y0: safe.y0 + 14 * u, y1: safe.y1 - 14 * u }; cols = 2; rows = 6; chipH = 96 * u; }
    else if (PORTRAIT) { box = { x0: safe.x0, x1: safe.x1, y0: TOP + 310 * u, y1: TOP + 310 * u + 640 * u }; cols = 2; rows = 6; chipH = 90 * u; }
    else { box = { x0: safe.x0, x1: safe.x1, y0: TOP + 190 * u, y1: TOP + 190 * u + 400 * u }; cols = 3; rows = 4; chipH = 86 * u; }
    const gapX = 16 * u;
    const gapY = (box.y1 - box.y0 - rows * chipH) / (rows - 1);
    const chipW = (box.x1 - box.x0 - gapX * (cols - 1)) / cols;
    const textW = LAND ? safe.w * 0.46 : safe.w;
    const tA = EV.devhead.t;
    const tB = EV.runson.t;
    kin(c, 'Built for', AX, TOP + kS * 0.6, kS, t, tA, { fill: PAPER, weight: 700, grp: 'dk' });
    const hs = fit('integrated GPUs.', textW, (PORTRAIT ? 150 : SQUARE ? 130 : 150) * u, 900);
    kin(c, 'integrated GPUs.', AX, TOP + kS * 1.25 + hs * 0.5, hs, t, tA + 0.08, { fill: 'ember', weight: 900, sweepAt: tA + 0.3, grp: 'dh' });
    // chips fly in from depth and land on the beat
    for (let i = 0; i < CHIPS.length; i++) {
      const tl = EV[`chip${i}`].t;
      const k = clamp((t - (tl - 0.5)) / 0.5);
      if (k <= 0) continue;
      const col = i % cols;
      const row = Math.floor(i / cols);
      const x = box.x0 + col * (chipW + gapX);
      const y = box.y0 + row * (chipH + gapY);
      const e = 1 - Math.pow(1 - k, 3);
      const bk = k < 1 ? e : 1;
      const sx0 = (R(i, 1) - 0.5) * W * 1.7;
      const sy0 = (R(i, 2) - 0.5) * H * 1.2;
      const cx0 = x + chipW / 2;
      const cy0 = y + chipH / 2;
      const px = lerp(cx0 + sx0, cx0, bk);
      const py = lerp(cy0 + sy0, cy0, bk);
      const sc = lerp(0.12, 1, bk);
      const rot = lerp((R(i, 3) - 0.5) * 2.4, 0, bk);
      const land = Math.exp(-Math.max(0, t - tl) / 0.2);
      c.save();
      c.translate(px, py);
      c.rotate(rot);
      c.scale(sc, sc);
      c.globalAlpha *= clamp(k * 2.2);
      rr(c, -chipW / 2, -chipH / 2, chipW, chipH, 18 * u);
      c.fillStyle = 'rgba(12,22,50,0.86)';
      c.fill();
      c.lineWidth = (2 + 3 * land) * u;
      c.strokeStyle = `rgba(${AMBER},${0.45 + 0.55 * land})`;
      c.stroke();
      if (land > 0.02) glow(c, 0, 0, chipW * 0.7, AMBER, 0.4 * land);
      const dotR = 7 * u;
      c.fillStyle = '#FFC94A';
      c.beginPath();
      c.arc(-chipW / 2 + 28 * u, 0, dotR, 0, Math.PI * 2);
      c.fill();
      const fs = fit(CHIPS[i], chipW - 82 * u, (PORTRAIT ? 36 : 38) * u, 800);
      str(c, CHIPS[i], -chipW / 2 + 50 * u, 0, { size: fs, weight: 800, fill: '#F4F5F7', align: 'left', grp: `chip${i}`, chk: k >= 1 });
      c.restore();
    }
    // Windows, Linux and macOS
    if (t >= tB) {
      const dt = t - tB;
      const lines = LAND ? [['Runs on Windows,'], ['Linux and macOS.']] : [['Runs on Windows,'], ['Linux and macOS.']];
      const os = fit('Linux and macOS.', textW, (PORTRAIT ? 78 : SQUARE ? 62 : 80) * u, 800);
      const y0 = LAND ? TOP + kS * 1.25 + hs + 90 * u : PORTRAIT ? box.y1 + 72 * u : box.y1 + 66 * u;
      lines.forEach((ln, i) => {
        const parts = ln[0].split(/(Windows|Linux|macOS)/).filter(Boolean).map((s) => ({ s, fill: /^(Windows|Linux|macOS)$/.test(s) ? '#FFC94A' : PAPER }));
        mixed(c, parts, AX, y0 + i * os * 1.2, { size: os, weight: 800, align: AL, sc: slamSc(dt - i * 0.08), alpha: popA(dt - i * 0.08), grp: `os${i}` });
      });
    }
    // honesty line
    const fa = popA(t - tA - 0.4, 0.3);
    str(c, 'Numbers in this film: Intel Arc 140T', LAND ? safe.x0 : CX, safe.y1 - 14 * u, { size: 23 * u, weight: 500, fam: MONO, fill: '#7C88A8', align: LAND ? 'left' : 'center', alpha: fa, grp: 'devnote' });
  }

  // ---------------------------------------------------------------- scene: plug and play
  const PAD_BODY = new Path2D('M200 92C260 80 380 80 440 92C520 96 568 120 590 178C612 236 624 318 600 362C580 398 530 404 506 372C482 340 470 304 440 296C380 282 260 282 200 296C170 304 158 340 134 372C110 404 60 398 40 362C16 318 28 236 50 178C72 120 120 96 200 92Z');
  function check(c, x, y, s, col, w) {
    c.save();
    c.strokeStyle = col;
    c.lineWidth = w;
    c.lineCap = 'round';
    c.lineJoin = 'round';
    c.beginPath();
    c.moveTo(x - 0.42 * s, y + 0.02 * s);
    c.lineTo(x - 0.1 * s, y + 0.32 * s);
    c.lineTo(x + 0.45 * s, y - 0.3 * s);
    c.stroke();
    c.restore();
  }
  function cursor(c, x, y, s, press) {
    c.save();
    c.translate(x, y);
    c.scale(s * (1 - 0.1 * press), s * (1 - 0.1 * press));
    c.fillStyle = '#fff';
    c.strokeStyle = '#0a0c11';
    c.lineWidth = 1.6;
    c.beginPath();
    c.moveTo(0, 0);
    c.lineTo(0, 24);
    c.lineTo(6, 18.5);
    c.lineTo(10, 27);
    c.lineTo(14, 25);
    c.lineTo(10, 17);
    c.lineTo(18, 17);
    c.closePath();
    c.fill();
    c.stroke();
    c.restore();
  }
  function sPlug(c, t) {
    G('plug');
    const tA = EV.oneclick.t;
    const tP = EV.press.t;
    const tPad = EV.pad.t;
    const kS = (PORTRAIT ? 150 : SQUARE ? 120 : 150) * u;
    const textW = LAND ? safe.w * 0.46 : safe.w;
    const vis = LAND
      ? { x0: safe.x0 + safe.w * 0.5, x1: safe.x1, y0: safe.y0, y1: safe.y1 }
      : { x0: safe.x0, x1: safe.x1, y0: TOP + (PORTRAIT ? 440 : 330) * u, y1: safe.y1 };
    const vcx = (vis.x0 + vis.x1) / 2;
    const vcy = (vis.y0 + vis.y1) / 2;
    if (t < tPad) {
      const ex = exitP(t, tPad);
      const hs = fit('One click.', textW, kS * 1.2, 900);
      kin(c, 'One click.', AX, TOP + hs * 0.55, hs, t, tA, { t1: tPad, fill: 'ember', weight: 900, sweepAt: tA + 0.2, grp: 'ph' });
      const cs = fit('It finds your games and keys', textW, (PORTRAIT ? 50 : SQUARE ? 42 : 48) * u, 600);
      const yC = TOP + hs * 1.1 + cs * 0.9;
      kin(c, 'It finds your games and keys', AX, yC, cs, t, tA + 0.5, { t1: tPad, mode: 'fade', fill: PAPER, weight: 600, grp: 'pc1' });
      kin(c, 'on your own PC.', AX, yC + cs * 1.2, cs, t, tA + 0.6, { t1: tPad, mode: 'fade', fill: PAPER, weight: 600, grp: 'pc2' });
      kin(c, 'It tunes itself.', AX, yC + cs * 2.6, cs * 1.1, t, tA + 1.0, { t1: tPad, mode: 'fade', fill: '#FFC94A', weight: 800, grp: 'pc3' });
      // installer card, flying in with a tilt
      const k = clamp((t - tA) / 0.6);
      const e = 1 - Math.pow(1 - k, 4);
      const cw = Math.min(vis.x1 - vis.x0, (LAND ? 800 : 780) * u);
      const chh = Math.min(vis.y1 - vis.y0 - 10 * u, (LAND ? 600 : 520) * u);
      const cy0 = LAND ? vcy : PORTRAIT ? vis.y0 + chh / 2 + 10 * u : vis.y0 + chh / 2;
      c.save();
      c.globalAlpha *= clamp(k * 3) * (1 - ex);
      c.translate(vcx - 360 * u * ex, cy0 + (1 - e) * 140 * u);
      const yaw = lerp(-0.5, -0.1, e) + 0.02 * Math.sin(t * 1.4);
      c.transform(Math.cos(yaw), Math.sin(yaw) * 0.16, 0, 1, 0, 0);
      c.scale(lerp(1.5, 1, e) * (1 - 0.15 * ex), lerp(1.5, 1, e) * (1 - 0.15 * ex));
      glow(c, 0, 0, cw * 0.8, ORANGE, 0.22);
      rr(c, -cw / 2, -chh / 2, cw, chh, 28 * u);
      c.fillStyle = 'rgba(10,18,42,0.94)';
      c.fill();
      c.strokeStyle = `rgba(${AMBER},0.55)`;
      c.lineWidth = 2.5 * u;
      c.stroke();
      // title bar
      drawLogo(c, -cw / 2 + 66 * u, -chh / 2 + 54 * u, 70 * u, { streaks: 0 });
      str(c, 'Guarana', -cw / 2 + 112 * u, -chh / 2 + 54 * u, { size: 38 * u, weight: 800, fill: PAPER, align: 'left', grp: 'cardt', chk: false });
      for (let i = 0; i < 3; i++) { c.fillStyle = 'rgba(160,175,215,0.35)'; c.beginPath(); c.arc(cw / 2 - 44 * u - i * 26 * u, -chh / 2 + 54 * u, 7 * u, 0, Math.PI * 2); c.fill(); }
      // button
      const bw = 360 * u;
      const bh = 96 * u;
      const by = -chh / 2 + 150 * u;
      const press = clamp(1 - (t - tP) / 0.18) * (t >= tP ? 1 : 0);
      const done = t >= EV.row3.t + 0.3;
      c.save();
      c.translate(0, by + bh / 2);
      c.scale(1 - 0.06 * press, 1 - 0.06 * press);
      rr(c, -bw / 2, -bh / 2, bw, bh, bh / 2);
      const bg = c.createLinearGradient(-bw / 2, 0, bw / 2, 0);
      if (done) { bg.addColorStop(0, '#3CCF7E'); bg.addColorStop(1, '#2BA865'); } else { bg.addColorStop(0, '#FFC94A'); bg.addColorStop(1, '#F2851A'); }
      c.fillStyle = bg;
      c.fill();
      glow(c, 0, 0, bw * 0.6, done ? GREEN : AMBER, 0.28);
      str(c, done ? 'Play' : t >= tP ? 'Setting up' : 'Install', 0, 0, { size: 42 * u, weight: 800, fill: '#0a0c11', grp: 'btn', chk: false });
      if (t >= tP) { const rp = (t - tP) / 0.5; if (rp < 1) ring(c, 0, 0, 30 * u + rp * 220 * u, 4 * u, WHITE, 0.6 * (1 - rp)); }
      c.restore();
      // rows
      const rows = [['Finds your games', EV.row1.t], ['Finds your keys', EV.row2.t], ['Tunes itself', EV.row3.t]];
      rows.forEach(([nm, tr], i) => {
        const ry = by + bh + 70 * u + i * 76 * u;
        const ra = popA(t - tP - i * 0.08, 0.2);
        c.save();
        c.globalAlpha *= ra;
        rr(c, -cw / 2 + 36 * u, ry - 30 * u, cw - 72 * u, 60 * u, 14 * u);
        c.fillStyle = 'rgba(30,44,86,0.55)';
        c.fill();
        str(c, nm, -cw / 2 + 62 * u, ry, { size: 33 * u, weight: 600, fill: '#DDE3F2', align: 'left', grp: 'row', chk: false });
        const cx = cw / 2 - 66 * u;
        if (t >= tr) {
          const cp = spring(t - tr, 3, 0.55);
          c.fillStyle = 'rgba(60,207,126,0.2)';
          c.beginPath();
          c.arc(cx, ry, 21 * u * Math.min(cp, 1.2), 0, Math.PI * 2);
          c.fill();
          check(c, cx, ry, 24 * u * Math.min(cp, 1.3), '#3CCF7E', 5 * u);
        } else if (t >= tP) {
          c.strokeStyle = `rgba(${AMBER},0.9)`;
          c.lineWidth = 4 * u;
          c.lineCap = 'round';
          c.beginPath();
          c.arc(cx, ry, 16 * u, t * 9 + i, t * 9 + i + 4.2);
          c.stroke();
        }
        c.restore();
      });
      // progress bar
      const pp = clamp((t - tP) / (EV.row3.t + 0.3 - tP));
      const py = by + bh + 70 * u + 3 * 76 * u - 10 * u;
      rr(c, -cw / 2 + 36 * u, py, cw - 72 * u, 12 * u, 6 * u);
      c.fillStyle = 'rgba(30,44,86,0.7)';
      c.fill();
      rr(c, -cw / 2 + 36 * u, py, Math.max(12 * u, (cw - 72 * u) * eIO(pp)), 12 * u, 6 * u);
      c.fillStyle = done ? '#3CCF7E' : '#FFC94A';
      c.fill();
      c.restore();
      // cursor flies to the button and clicks
      const cp2 = clamp((t - (tP - 0.55)) / 0.55);
      if (t > tP - 0.6 && !done) {
        const bx = vcx - 360 * u * ex;
        const ey = cy0 + by + bh / 2 + 24 * u;
        const e2 = 1 - Math.pow(1 - cp2, 3);
        cursor(c, lerp(bx + 380 * u, bx + 40 * u, e2), lerp(ey + 360 * u, ey, e2), 2.1 * u, press);
      }
    } else {
      // controller: "Your controller just works."
      const dt = t - tPad;
      const hs = fit('Your controller', textW, (PORTRAIT ? 120 : SQUARE ? 100 : 118) * u, 900);
      kin(c, 'Your controller', AX, TOP + hs * 0.55, hs, t, tPad, { fill: PAPER, weight: 900, grp: 'cq1' });
      kin(c, 'just works.', AX, TOP + hs * 1.55, hs * 1.08, t, tPad + 0.1, { fill: 'ember', weight: 900, sweepAt: tPad + 0.3, grp: 'cq2' });
      const sub = fit('Player 1 sets itself up when it connects.', textW, 38 * u, 600);
      kin(c, 'Player 1 sets itself up when it connects.', AX, TOP + hs * 2.3 + sub, sub, t, EV.padplug.t, { mode: 'fade', fill: '#C9D2EA', weight: 600, grp: 'cq3' });
      const ps = Math.min((vis.x1 - vis.x0) / 700, (vis.y1 - vis.y0) / 560) * 0.9;
      const px = vcx - 320 * ps;
      const py = vcy - 200 * ps + (LAND ? 0 : 20 * u);
      const drop = clamp(dt / 0.25);
      const plugged = t >= EV.padplug.t;
      c.save();
      c.globalAlpha *= clamp(dt / 0.15);
      c.translate(px, py);
      c.scale(ps, ps);
      // cable drops in and plugs
      const cableY = lerp(-260, 0, plugged ? 1 : eOut(clamp((t - tPad - 0.05) / 0.2)));
      c.strokeStyle = '#6E7794';
      c.lineWidth = 12;
      c.lineCap = 'round';
      c.beginPath();
      c.moveTo(320, Math.min(-100, (vis.y0 - py) / ps));
      c.lineTo(320, cableY + 22);
      c.stroke();
      rr(c, 300, cableY + 22, 40, 44, 9);
      c.fillStyle = '#9AA3BD';
      c.fill();
      c.save();
      c.translate(0, 0);
      glow(c, 320, 200, 520, ORANGE, 0.18 * clamp(dt / 0.4));
      c.fillStyle = 'rgba(12,20,46,0.96)';
      c.fill(PAD_BODY);
      c.strokeStyle = `rgba(${AMBER},0.95)`;
      c.lineWidth = 7;
      c.lineJoin = 'round';
      c.stroke(PAD_BODY);
      // sticks, d-pad, buttons
      c.fillStyle = 'rgba(70,86,130,0.9)';
      c.fillRect(140, 150, 70, 22);
      c.fillRect(162, 128, 26, 66);
      for (const [bx, by2] of [[465, 150], [500, 185], [465, 220], [430, 185]]) { c.beginPath(); c.arc(bx, by2, 15, 0, Math.PI * 2); c.fillStyle = 'rgba(255,201,74,0.85)'; c.fill(); }
      for (const sx of [252, 388]) { c.beginPath(); c.arc(sx, 250, 34, 0, Math.PI * 2); c.fillStyle = 'rgba(40,54,96,1)'; c.fill(); c.beginPath(); c.arc(sx, 250, 19, 0, Math.PI * 2); c.fillStyle = 'rgba(90,108,160,1)'; c.fill(); }
      // player LEDs
      for (let i = 0; i < 4; i++) {
        const lx = 276 + i * 30;
        const lit = plugged && i === 0;
        c.beginPath();
        c.arc(lx, 120, 7, 0, Math.PI * 2);
        c.fillStyle = lit ? '#FFE08A' : 'rgba(110,125,170,0.6)';
        c.fill();
        if (lit) glow(c, lx, 120, 60, AMBER, 0.9 * Math.min(1, spring(t - EV.padplug.t, 2.5, 0.5)));
      }
      c.restore();
      if (plugged) ring(c, 320, 66, 20 + (t - EV.padplug.t) * 600, 8, AMBER, 0.7 * Math.max(0, 1 - (t - EV.padplug.t) / 0.6));
      c.restore();
      void drop;
    }
  }

  // ---------------------------------------------------------------- scene: AI-native
  const AGENTS = ['Claude Code', 'Codex', 'Gemini CLI', 'Cursor', 'Copilot'];
  function sAI(c, t) {
    G('ai');
    const tA = EV.aihead.t;
    const tKey0 = EV.type0.t;
    const tKey1 = EV.type0.t1;
    const tEnter = EV.enter.t;
    const tGreen = EV.green.t;
    const kS = (PORTRAIT ? 112 : SQUARE ? 66 : 108) * u;
    const textW = LAND ? safe.w * 0.44 : safe.w;
    const vis = LAND
      ? { x0: safe.x0 + safe.w * 0.48, x1: safe.x1, y0: safe.y0, y1: safe.y1 }
      : { x0: safe.x0, x1: safe.x1, y0: TOP + (PORTRAIT ? 430 : SQUARE ? 225 : 300) * u, y1: safe.y1 };
    const vw = vis.x1 - vis.x0;
    const vh = vis.y1 - vis.y0;
    const s1 = fit('Plug in your', textW, kS, 800);
    const s2 = fit('AI agent.', textW, kS * 1.2, 900);
    kin(c, 'Plug in your', AX, TOP + s1 * 0.55, s1, t, tA, { fill: PAPER, weight: 800, grp: 'a1' });
    kin(c, 'AI agent.', AX, TOP + s1 * 1.1 + s2 * 0.52, s2, t, tA + 0.08, { fill: 'ember', weight: 900, sweepAt: tA + 0.3, grp: 'a2' });
    const s3 = fit('Ship a fix tonight.', textW, kS * 0.78, 900);
    kin(c, 'Ship a fix tonight.', AX, TOP + s1 * 1.1 + s2 * 1.04 + s3 * 0.7, s3, t, tGreen, { fill: '#7BE6A8', weight: 900, sweepAt: tGreen + 0.1, grp: 'a3' });
    // terminal
    const cmd = 'claude "fix issue #42"';
    const fs = Math.min((LAND ? 40 : SQUARE ? 30 : 34) * u, (vw - 110 * u) / tw(cmd, 1, 600, MONO));
    const th = (SQUARE ? 70 : 84) * u + fs * 4.85;
    const tk = clamp((t - tA - 0.1) / 0.5);
    const te = 1 - Math.pow(1 - tk, 3);
    c.save();
    c.globalAlpha *= clamp(tk * 3);
    c.translate(0, (1 - te) * 90 * u);
    const tx = vis.x0;
    const ty = vis.y0;
    rr(c, tx, ty, vw, th, 20 * u);
    c.fillStyle = 'rgba(6,10,24,0.94)';
    c.fill();
    c.strokeStyle = `rgba(${AMBER},0.4)`;
    c.lineWidth = 2 * u;
    c.stroke();
    for (let i = 0; i < 3; i++) { c.fillStyle = ['#FF6B4A', '#FFC94A', '#3CCF7E'][i]; c.globalAlpha *= 0.85; c.beginPath(); c.arc(tx + 36 * u + i * 30 * u, ty + 34 * u, 8 * u, 0, Math.PI * 2); c.fill(); c.globalAlpha /= 0.85; }
    const nChars = Math.floor(clamp((FT - tKey0) / (tKey1 - tKey0)) * cmd.length + (FT >= tKey0 ? 0.999 : 0));
    const shown = cmd.slice(0, Math.min(cmd.length, nChars));
    const ly = ty + (SQUARE ? 70 : 84) * u;
    str(c, '>', tx + 40 * u, ly, { size: fs, weight: 700, fam: MONO, fill: '#FFC94A', align: 'left', grp: 'term', chk: false });
    str(c, shown, tx + 40 * u + fs * 1.3, ly, { size: fs, weight: 600, fam: MONO, fill: '#F4F5F7', align: 'left', grp: 'term', chk: false });
    if (t < tEnter + 0.1 && Math.floor(t * 5) % 2 === 0) {
      c.fillStyle = '#FFC94A';
      c.fillRect(tx + 40 * u + fs * 1.3 + tw(shown, fs, 600, MONO) + 2 * u, ly - fs * 0.5, fs * 0.55, fs);
    }
    const outs = ['reading AGENTS.md', 'writing the fix', 'opening a pull request'];
    outs.forEach((o2, i) => {
      const to = tEnter + 0.05 + i * 0.16;
      if (t < to) return;
      const oy = ly + fs * 1.5 + i * fs * 1.3;
      const oa = popA(t - to, 0.12);
      c.save();
      c.globalAlpha *= oa;
      check(c, tx + 52 * u, oy, 20 * u, '#3CCF7E', 4 * u);
      str(c, o2, tx + 40 * u + fs * 1.3, oy, { size: fs * 0.82, weight: 500, fam: MONO, fill: '#AEB9D6', align: 'left', grp: 'term', chk: false });
      c.restore();
    });
    c.restore();
    if (t >= tEnter) { const d = t - tEnter; ring(c, tx + vw / 2, ty + th / 2, 40 * u + d * 1400 * u, 5 * u, AMBER, 0.5 * Math.max(0, 1 - d / 0.4)); }
    // agent chips
    const chipFs = Math.min((SQUARE ? 26 : 30) * u, vw / 15);
    const cy0 = ty + th + 26 * u + chipFs * 1.1;
    let rx = vis.x0;
    let ry = cy0;
    const ph = chipFs * 2.1;
    AGENTS.forEach((nm, i) => {
      const w = tw(nm, chipFs, 700) + 42 * u;
      if (rx + w > vis.x1 + 1) { rx = vis.x0; ry += ph + 12 * u; }
      const ta = EV[`agent${i}`].t;
      const dt = t - ta;
      if (dt >= 0) {
        const sc = Math.min(1.08, spring(dt, 3, 0.5));
        c.save();
        c.translate(rx + w / 2, ry);
        c.scale(sc, sc);
        c.globalAlpha *= popA(dt, 0.06);
        rr(c, -w / 2, -ph / 2, w, ph, ph / 2);
        c.fillStyle = 'rgba(255,201,74,0.1)';
        c.fill();
        c.strokeStyle = `rgba(${AMBER},${0.5 + 0.5 * Math.exp(-dt / 0.2)})`;
        c.lineWidth = 2.5 * u;
        c.stroke();
        str(c, nm, 0, 0, { size: chipFs, weight: 700, fill: '#FFE9B0', grp: `ag${i}`, chk: dt > 0.5 });
        c.restore();
      }
      rx += w + 12 * u;
    });
    const rowsUsed = ry;
    // paste line
    const tp = EV.paste.t;
    const pasteTxt = 'or paste it into ChatGPT / Claude on the web';
    const pfs = Math.min(29 * u, vw / (pasteTxt.length * 0.62));
    const pyy = rowsUsed + ph / 2 + 38 * u;
    if (t >= tp) str(c, pasteTxt, vis.x0, pyy, { size: pfs, weight: 500, fam: MONO, fill: '#9FB0D6', align: 'left', alpha: popA(t - tp, 0.25), grp: 'paste' });
    // pull request card
    const prY = pyy + 40 * u;
    const prH = clamp(vis.y1 - prY, 170 * u, 250 * u);
    const prT = tp + 0.0;
    const pk = clamp((t - prT) / 0.45);
    if (pk > 0 && prH > 120 * u) {
      const pe = 1 - Math.pow(1 - pk, 3);
      const green = t >= tGreen;
      const gd = t - tGreen;
      c.save();
      c.globalAlpha *= clamp(pk * 3);
      c.translate((1 - pe) * 120 * u, 0);
      rr(c, vis.x0, prY, vw, prH, 20 * u);
      c.fillStyle = 'rgba(8,16,36,0.95)';
      c.fill();
      c.strokeStyle = green ? `rgba(${GREEN},0.9)` : `rgba(${AMBER},0.35)`;
      c.lineWidth = (green ? 3 : 2) * u;
      c.stroke();
      if (green) glow(c, vis.x0 + vw / 2, prY + prH / 2, vw * 0.55, GREEN, 0.18 * Math.exp(-gd / 0.8) + 0.07);
      // merge icon
      c.strokeStyle = green ? '#3CCF7E' : '#FFC94A';
      c.lineWidth = 4 * u;
      c.lineCap = 'round';
      const ix = vis.x0 + 46 * u;
      const iy = prY + 54 * u;
      c.beginPath();
      c.arc(ix, iy - 12 * u, 8 * u, 0, Math.PI * 2);
      c.moveTo(ix, iy - 4 * u);
      c.lineTo(ix, iy + 24 * u);
      c.moveTo(ix + 24 * u, iy + 30 * u);
      c.arc(ix + 24 * u, iy + 24 * u, 6 * u, 0, Math.PI * 2);
      c.stroke();
      str(c, 'Fix issue #42', vis.x0 + 82 * u, prY + 46 * u, { size: 36 * u, weight: 800, fill: PAPER, align: 'left', grp: 'pr' });
      const status = green ? 'All checks passed' : 'Checks running';
      str(c, status, vis.x0 + 82 * u, prY + 46 * u + 56 * u, { size: 29 * u, weight: 600, fam: MONO, fill: green ? '#7BE6A8' : '#FFD36B', align: 'left', grp: 'pr' });
      // three checks
      const labels = ['build', 'tests', 'review'];
      const cw3 = (vw - 60 * u) / 3;
      labels.forEach((lb, i) => {
        const cx = vis.x0 + 30 * u + i * cw3;
        const cyy = prY + prH - 50 * u;
        const done = t >= tGreen - 0.18 + i * 0.08 && t >= tGreen - 0.0 - (2 - i) * 0.0 && t >= tGreen - 0.0;
        void done;
        const ok = t >= tGreen + i * 0.06;
        rr(c, cx, cyy - 28 * u, cw3 - 14 * u, 56 * u, 14 * u);
        c.fillStyle = ok ? 'rgba(60,207,126,0.16)' : 'rgba(255,201,74,0.08)';
        c.fill();
        if (ok) check(c, cx + 30 * u, cyy, 20 * u, '#3CCF7E', 4 * u);
        else { c.strokeStyle = `rgba(${AMBER},0.9)`; c.lineWidth = 3.5 * u; c.beginPath(); c.arc(cx + 30 * u, cyy, 10 * u, t * 9 + i, t * 9 + i + 4); c.stroke(); }
        str(c, lb, cx + 56 * u, cyy, { size: 26 * u, weight: 600, fam: MONO, fill: '#DDE3F2', align: 'left', grp: `chk${i}`, chk: false });
      });
      c.restore();
      if (green) sparks(c, t, tGreen, vis.x0 + vw / 2, prY + prH / 2, 40, 900, 0.9, 11, GREEN);
    }
  }

  // ---------------------------------------------------------------- scene: legacy
  function sLegacy(c, t, S) {
    G('legacy');
    const t0 = EV.yourname.t;
    const t1 = EV.inrepo.t;
    const t2 = EV.forever.t;
    const textW = LAND ? safe.w * 0.46 : safe.w;
    const s = fit('In the repo.', textW, (PORTRAIT ? 150 : SQUARE ? 112 : 148) * u, 900);
    const lh = s * 1.12;
    const y0 = TOP + s * 0.5;
    kin(c, 'Your name.', AX, y0, s, t, t0, { fill: PAPER, weight: 900, grp: 'l1' });
    kin(c, 'In the repo.', AX, y0 + lh, s, t, t1, { fill: PAPER, weight: 900, grp: 'l2' });
    kin(c, 'Forever.', AX, y0 + lh * 2, s * 1.05, t, t2, { fill: 'ember', weight: 900, sweepAt: t2 + 0.1, grp: 'l3' });
    const vis = LAND
      ? { x0: safe.x0 + safe.w * 0.5, x1: safe.x1, y0: safe.y0 + 10 * u, y1: safe.y1 - 10 * u }
      : { x0: safe.x0, x1: safe.x1, y0: TOP + lh * 3 + 50 * u, y1: safe.y1 };
    const ts = Math.min(PORTRAIT ? 96 * u : SQUARE ? 78 * u : 100 * u, (vis.x1 - vis.x0) / 7.5);
    const pitch = ts * 1.18;
    const cols = Math.max(3, Math.floor((vis.x1 - vis.x0 + (pitch - ts)) / pitch) | 1);
    const rowsN = Math.max(3, Math.floor((vis.y1 - vis.y0 + (pitch - ts)) / pitch) | 1);
    const gx0 = (vis.x0 + vis.x1) / 2 - ((cols - 1) * pitch) / 2;
    const gy0 = (vis.y0 + vis.y1) / 2 - ((rowsN - 1) * pitch) / 2;
    const mc = (cols - 1) / 2;
    const mr = (rowsN - 1) / 2;
    const youT = t1;
    const letters = 'ABCDEFGHJKLMNPRSTVWXYZ';
    const pal = [[42, 74, 150], [34, 96, 130], [86, 66, 140], [120, 80, 50], [40, 110, 100], [70, 90, 170]];
    const order = [];
    for (let r = 0; r < rowsN; r++) for (let q = 0; q < cols; q++) order.push([q, r]);
    const total = order.length;
    // appear order: random permutation, finishing before the @you slam
    const idxs = order.map((_, i) => i).sort((a, b) => R(a, 21) - R(b, 21));
    const allLit = clamp((t - t2) / 0.3);
    idxs.forEach((oi, rank) => {
      const [q, r] = order[oi];
      if (q === Math.round(mc) && r === Math.round(mr)) return;
      const ta = t0 + 0.05 + (rank / total) * 0.55;
      const dt = t - ta;
      if (dt < 0) return;
      const x = gx0 + q * pitch;
      const y = gy0 + r * pitch;
      const dist = Math.hypot(q - mc, r - mr);
      const wave = Math.exp(-Math.pow((t - youT - dist * 0.07) / 0.1, 2));
      const sc = Math.min(1.12, spring(dt, 3.2, 0.55)) * (1 + 0.12 * wave * (t >= youT ? 1 : 0));
      const pc = pal[Math.floor(R(oi, 31) * pal.length)];
      c.save();
      c.translate(x, y);
      c.scale(sc, sc);
      c.globalAlpha *= popA(dt, 0.08);
      rr(c, -ts / 2, -ts / 2, ts, ts, ts * 0.24);
      const lg = c.createLinearGradient(-ts / 2, -ts / 2, ts / 2, ts / 2);
      lg.addColorStop(0, `rgb(${pc[0] + 30},${pc[1] + 30},${pc[2] + 30})`);
      lg.addColorStop(1, `rgb(${pc[0]},${pc[1]},${pc[2]})`);
      c.fillStyle = lg;
      c.fill();
      c.lineWidth = 2 * u;
      c.strokeStyle = `rgba(${AMBER},${0.18 + 0.7 * Math.max(wave, allLit * 0.55)})`;
      c.stroke();
      str(c, letters[Math.floor(R(oi, 41) * letters.length)] + letters[Math.floor(R(oi, 42) * letters.length)].toLowerCase(), 0, 0, { size: ts * 0.38, weight: 800, fill: 'rgba(235,240,255,0.85)', grp: 'tile', chk: false });
      if (wave > 0.05) glow(c, 0, 0, ts * 1.2, AMBER, 0.5 * wave);
      c.restore();
    });
    // @you lands in the middle on "In the repo."
    const dy = t - youT;
    if (dy >= 0) {
      const sc = lerp(2.6, 1.3, Math.min(1, spring(dy, 2.6, 0.5)));
      const x = gx0 + Math.round(mc) * pitch;
      const y = gy0 + Math.round(mr) * pitch;
      c.save();
      c.translate(x, y);
      c.scale(sc, sc);
      c.globalAlpha *= popA(dy, 0.05);
      glow(c, 0, 0, ts * 2.4, AMBER, 0.55);
      rr(c, -ts / 2, -ts / 2, ts, ts, ts * 0.24);
      const lg = c.createLinearGradient(-ts / 2, -ts / 2, ts / 2, ts / 2);
      lg.addColorStop(0, '#FFD36B');
      lg.addColorStop(1, '#F2851A');
      c.fillStyle = lg;
      c.fill();
      str(c, '@you', 0, 0, { size: ts * 0.3, weight: 800, fam: MONO, fill: '#14100a', grp: 'you', chk: false });
      c.restore();
      ring(c, x, y, ts + dy * 1100 * u, 5 * u, AMBER, 0.7 * Math.max(0, 1 - dy / 0.7));
      sparks(c, t, youT, x, y, 46, 800, 0.9, 51, AMBER);
    }
    void S;
  }

  // ---------------------------------------------------------------- scene: call to action
  function sCta(c, t) {
    G('cta');
    const t0 = EV.cut_cta.t;
    const dt = t - t0;
    const logoSize = (PORTRAIT ? 420 : SQUARE ? 250 : 520) * u;
    const lx = LAND ? safe.x0 + safe.w * 0.22 : CX;
    const ly = LAND ? CY - 20 * u : TOP + logoSize * 0.5 + 10 * u;
    // light burst
    const bst = Math.exp(-dt / 0.35);
    glow(c, lx, ly, Math.max(W, H) * (0.4 + 0.5 * eOut(dt / 0.5)), WHITE, 0.3 * Math.exp(-dt / 0.1));
    glow(c, lx, ly, Math.max(W, H) * 0.7, ORANGE, 0.3 * bst + 0.14);
    ring(c, lx, ly, 20 * u + eOutE(dt / 0.9) * Math.max(W, H) * 0.9, 10 * u, AMBER, 0.8 * Math.max(0, 1 - dt / 0.9));
    ring(c, lx, ly, 20 * u + eOutE((dt - 0.1) / 0.9) * Math.max(W, H) * 0.7, 4 * u, WHITE, 0.6 * Math.max(0, 1 - (dt - 0.1) / 0.8));
    // rays
    c.save();
    c.globalCompositeOperation = 'lighter';
    c.translate(lx, ly);
    for (let i = 0; i < 18; i++) {
      const a = (i / 18) * Math.PI * 2 + t * 0.12 + R(i, 3) * 0.3;
      const len = Math.max(W, H) * (0.35 + 0.5 * R(i, 4));
      c.save();
      c.rotate(a);
      c.globalAlpha = Math.min(1, 0.5 * Math.exp(-dt / 0.9) + 0.06) * (0.5 + 0.5 * R(i, 5));
      c.drawImage(shaft, -len * 0.04, 0, len * 0.08, len);
      c.restore();
    }
    c.restore();
    sparks(c, t, t0, lx, ly, 90, 1500, 1.4, 61, AMBER);
    sparks(c, t, t0 + 0.03, lx, ly, 40, 900, 1.2, 81, WHITE);
    // logo resolves: streaks fly in, the drop lands, then leaf, then the three echo arcs
    const k = clamp(dt / 0.45);
    const ls = lerp(1.6, 1, spring(dt, 2.2, 0.62));
    const dropA = clamp(dt / 0.08);
    const leaf = Math.min(1.3, spring(dt - 0.3, 3, 0.5));
    const arcs = [0, 1, 2].map((i) => clamp((dt - 0.38 - i * 0.1) / 0.12));
    c.save();
    c.translate(lx, ly);
    c.scale(ls, ls);
    c.translate(-lx, -ly);
    drawLogo(c, lx + 14 * (logoSize / 420) * 0 , ly, logoSize, { streaks: k, drop: dropA, leaf: Math.max(0, leaf), arcs });
    c.restore();
    // text
    const textW = LAND ? safe.w * 0.56 : safe.w;
    const tx = LAND ? safe.x0 + safe.w * 0.44 : CX;
    const al = LAND ? 'left' : 'center';
    const wm = fit('Guarana', textW, (PORTRAIT ? 150 : SQUARE ? 100 : 140) * u, 900);
    const wy = LAND ? CY - 280 * u : ly + logoSize * 0.5 + wm * 0.62 + 20 * u;
    kin(c, 'Guarana', tx, wy, wm, t, t0 + 0.25, { align: al, fill: PAPER, weight: 900, sweepAt: t0 + 0.35, grp: 'wm' });
    const tM = EV.mission.t;
    const ms = fit('Join the mission.', textW, (PORTRAIT ? 124 : SQUARE ? 84 : 124) * u, 900);
    const my = wy + wm * 0.5 + ms * 0.8;
    kin(c, 'Join the mission.', tx, my, ms, t, tM, { align: al, fill: 'ember', weight: 900, sweepAt: tM + 0.1, grp: 'jm' });
    // the two links
    const urls = [['Discord', 'discord.gg/urz7vMdRM', EV.discord.t], ['GitHub', 'github.com/karimalsalah/guarana', EV.github.t]];
    const uFs = Math.min(38 * u, (textW - 60 * u) / tw(urls[1][1], 1, 700, MONO));
    let uy = my + ms * 0.5 + uFs * 2.1;
    urls.forEach(([, url, tu], i) => {
      const d = t - tu;
      if (d < 0) { uy += uFs * 3.1; return; }
      const w = tw(url, uFs, 700, MONO) + 60 * u;
      const px = LAND ? tx : tx - w / 2;
      c.save();
      c.translate(px + w / 2, uy);
      const sc = Math.min(1.06, spring(d, 3, 0.55));
      c.scale(sc, sc);
      c.globalAlpha *= popA(d, 0.06);
      rr(c, -w / 2, -uFs * 1.15, w, uFs * 2.3, uFs * 1.15);
      c.fillStyle = 'rgba(255,201,74,0.1)';
      c.fill();
      c.strokeStyle = `rgba(${AMBER},${0.6 + 0.4 * Math.exp(-d / 0.25)})`;
      c.lineWidth = 2.5 * u;
      c.stroke();
      glow(c, 0, 0, w * 0.55, AMBER, 0.14 * Math.exp(-d / 0.5));
      str(c, url, 0, 0, { size: uFs, weight: 700, fam: MONO, fill: '#FFE9B0', grp: `url${i}`, sweep: (d - 0.05) / 0.6 });
      c.restore();
      uy += uFs * 3.1;
    });
    str(c, 'Open source. Not affiliated with Nintendo. Ships no games.', tx, safe.y1 - 14 * u, { size: 23 * u, weight: 500, fam: MONO, fill: '#7C88A8', align: al, alpha: popA(t - 19.4, 0.4), grp: 'fine' });
  }

  // ---------------------------------------------------------------- watermark
  function watermark(c, t) {
    if (t < 2 || t >= 18) return;
    const x = PORTRAIT ? CX : safe.x0;
    const y = PORTRAIT ? safe.y0 - 62 * u : safe.y0 + 20 * u;
    c.save();
    c.globalAlpha = 0.85;
    const w = tw('Guarana', 34 * u, 800);
    const x0 = PORTRAIT ? x - (w + 52 * u) / 2 : x;
    drawLogo(c, x0 + 22 * u, y, 46 * u, {});
    str(c, 'Guarana', x0 + 52 * u, y, { size: 34 * u, weight: 800, fill: '#DDE3F2', align: 'left', grp: 'wm', chk: false });
    c.restore();
  }

  // ---------------------------------------------------------------- frame assembly
  const DRAW = { open: sOpen, problem: sProblem, fix: sFix, devices: sDevices, plug: sPlug, ai: sAI, legacy: sLegacy, cta: sCta };

  function drawFg(c, t, S) {
    c.save();
    c.globalCompositeOperation = 'source-over';
    c.globalAlpha = 1;
    const cam = camera(t, S);
    c.save();
    applyCam(c, cam, 1);
    DRAW[S.id](c, t, S);
    c.restore();
    // near dust in front of everything, with stronger parallax
    c.save();
    applyCam(c, cam, 1.6);
    if (S.id !== 'open') motes(c, t, Math.round(26 * (W * H) / (1080 * 1920) + 14), 40, 0.86, 1, cam.x);
    c.restore();
    // sparks at every hard cut
    for (const e of KIND.boom || []) {
      if (e.t === S.start && S.id !== 'cta') sparks(c, t, e.t, CX, CY, 70, 1300 * e.v, 0.9, Math.round(e.t * 10), AMBER);
    }
    c.restore();
  }

  function post(t, S) {
    const c = octx;
    c.globalCompositeOperation = 'source-over';
    c.globalAlpha = 1;
    c.filter = 'none';
    c.drawImage(acc, 0, 0);
    // bloom: bright pass at 1/4, blurred at 1/8 and 1/16, added back
    b1c.globalCompositeOperation = 'copy';
    b1c.filter = 'brightness(0.85) contrast(2.6)';
    b1c.drawImage(acc, 0, 0, b1.width, b1.height);
    b1c.filter = 'none';
    b2c.globalCompositeOperation = 'copy';
    b2c.filter = 'blur(4px)';
    b2c.drawImage(b1, 0, 0, b2.width, b2.height);
    b2c.filter = 'none';
    b3c.globalCompositeOperation = 'copy';
    b3c.filter = 'blur(3px)';
    b3c.drawImage(b2, 0, 0, b3.width, b3.height);
    b3c.filter = 'none';
    let hits = 0;
    for (const e of KIND.boom || []) { const d = t - e.t; if (d >= 0) hits += e.v * Math.exp(-d / 0.1); }
    for (const e of KIND.hit || []) { const d = t - e.t; if (d >= 0) hits += 0.35 * e.v * Math.exp(-d / 0.08); }
    const lv = level(t);
    c.save();
    c.imageSmoothingEnabled = true;
    c.imageSmoothingQuality = 'high';
    c.globalCompositeOperation = 'lighter';
    const bl = S && S.id === 'cta' ? 0.5 : 1;
    c.globalAlpha = Math.min(1, (0.2 + 0.1 * lv + 0.14 * Math.min(hits, 1.5)) * bl);
    c.drawImage(b2, 0, 0, W, H);
    c.globalAlpha = Math.min(1, (0.22 + 0.14 * Math.min(hits, 1.5)) * bl);
    c.drawImage(b3, 0, 0, W, H);
    // anamorphic streak: the bright pass smeared sideways
    skc.globalCompositeOperation = 'copy';
    skc.globalAlpha = 1;
    skc.drawImage(b2, 0, 0);
    skc.globalCompositeOperation = 'lighter';
    for (let k = 1; k <= 14; k++) {
      const a = 0.16 * Math.exp(-k / 5.5);
      skc.globalAlpha = a;
      skc.drawImage(b2, k * 4, 0);
      skc.drawImage(b2, -k * 4, 0);
    }
    skc.globalAlpha = 1;
    c.globalAlpha = Math.min(1, (0.16 + 0.22 * Math.min(hits, 1.5)) * bl);
    c.drawImage(sk, 0, 0, W, H);
    c.restore();
    // chromatic split on hard cuts and glitches
    let chroma = 0;
    for (const e of KIND.boom || []) { const d = t - e.t; if (d >= -0.02 && d < 0.5) chroma += 16 * e.v * Math.exp(-Math.max(0, d) / 0.07); }
    for (const e of KIND.glitch || []) { const d = t - e.t; if (d >= 0 && d < 0.5) chroma += 9 * e.v * Math.exp(-d / 0.09); }
    chroma *= u;
    if (chroma > 0.6) {
      tctx.globalCompositeOperation = 'copy';
      tctx.drawImage(out, 0, 0);
      c.globalCompositeOperation = 'source-over';
      c.fillStyle = '#000';
      c.fillRect(0, 0, W, H);
      const chans = [['#ff0000', -chroma, -chroma * 0.15], ['#00ff00', 0, 0], ['#0000ff', chroma, chroma * 0.15]];
      for (const [col, dx, dy] of chans) {
        chctx.globalCompositeOperation = 'copy';
        chctx.drawImage(tmp, 0, 0);
        chctx.globalCompositeOperation = 'multiply';
        chctx.fillStyle = col;
        chctx.fillRect(0, 0, W, H);
        c.globalCompositeOperation = 'lighter';
        c.drawImage(chn, dx, dy);
      }
    }
    c.globalCompositeOperation = 'source-over';
    // vignette
    const vg = c.createRadialGradient(CX, CY, Math.min(W, H) * 0.3, CX, CY, Math.hypot(W, H) * 0.62);
    vg.addColorStop(0, 'rgba(2,4,10,0)');
    vg.addColorStop(1, 'rgba(2,4,10,0.72)');
    c.fillStyle = vg;
    c.fillRect(0, 0, W, H);
    // flash on hard cuts
    let fl = 0;
    for (const e of KIND.boom || []) { const d = t - e.t; if (d >= 0) fl += 0.3 * e.v * Math.exp(-d / 0.035); }
    if (fl > 0.01) {
      c.globalCompositeOperation = 'lighter';
      c.fillStyle = `rgba(255,214,150,${Math.min(0.5, fl)})`;
      c.fillRect(0, 0, W, H);
      c.globalCompositeOperation = 'source-over';
    }
    void S;
  }

  // ---------------------------------------------------------------- thumbnail (static)
  function drawThumb(c) {
    const t = 7.4;
    const S = { id: 'fix', start: 4, end: 8 };
    drawBg(c, t, S);
    c.save();
    G('thumb');
    const pad = safe.x0;
    // frame-time line flattening onto the rail, along the bottom
    const gy = H - 150 * u;
    c.save();
    c.lineCap = 'round';
    c.lineJoin = 'round';
    const path = new Path2D();
    for (let x = pad; x <= W - pad; x += 4 * u) {
      const f = (x - pad) / (W - 2 * pad);
      const amp = 1 - sstep(0.15, 0.7, f);
      const y = gy - amp * (vn(f * 9 + 2) * 60 * u + (R(Math.floor(f * 22), 3) > 0.62 ? 90 * u : 0));
      x === pad ? path.moveTo(x, y) : path.lineTo(x, y);
    }
    c.globalCompositeOperation = 'lighter';
    c.strokeStyle = 'rgba(255,170,60,0.3)';
    c.lineWidth = 22 * u;
    c.stroke(path);
    c.strokeStyle = '#FFC94A';
    c.lineWidth = 7 * u;
    c.stroke(path);
    c.restore();
    shaftsOnly(c);
    str(c, 'Ocarina of Time on a laptop', pad, safe.y0 + 40 * u, { size: 50 * u, weight: 700, fill: '#E6EAF5', align: 'left', grp: 'th1' });
    const hs = Math.min(420 * u, (W - 2 * pad) * 0.62 / (tw('22.8', 1, 900) + 0.12 + tw('fps', 0.4, 800)));
    const hy = CY - 20 * u;
    const wN = tw('22.8', hs, 900);
    str(c, '22.8', pad, hy, { size: hs, weight: 900, fill: 'ember', align: 'left', grp: 'th2' });
    str(c, 'fps', pad + wN + 0.12 * hs, hy + hs * 0.14, { size: 0.4 * hs, weight: 900, fill: PAPER, align: 'left', grp: 'th3' });
    str(c, 'was 14.8 fps. One shader fix.', pad, hy + hs * 0.62, { size: 44 * u, weight: 700, fill: '#C9D2EA', align: 'left', grp: 'th4' });
    drawLogo(c, W - pad - 150 * u, safe.y0 + 150 * u, 280 * u, { streaks: 1 });
    str(c, 'Guarana', W - pad, safe.y0 + 330 * u, { size: 72 * u, weight: 900, fill: PAPER, align: 'right', grp: 'th5' });
    str(c, 'open source, for integrated GPUs', W - pad, safe.y0 + 400 * u, { size: 28 * u, weight: 600, fam: MONO, fill: '#9FB0D6', align: 'right', grp: 'th6' });
    c.restore();
  }
  function shaftsOnly(c) {
    drawShafts(c, 3, W * 0.9, -0.1 * H, CX, CY, 7, 0.1, 0.5);
  }

  // ---------------------------------------------------------------- renderAt
  let lastT = 0;
  function renderAt(tIn) {
    const t = clamp(Number(tIn) || 0, 0, DUR - 1e-6);
    lastT = t;
    FT = t;
    if (THUMB) {
      drawThumb(sctx);
      octx.globalCompositeOperation = 'source-over';
      octx.drawImage(scn, 0, 0);
      actx.drawImage(scn, 0, 0);
      post(7.4, STYLE.fix);
      return true;
    }
    const S0 = sceneAt(t);
    drawBg(bctx, t, S0);
    for (let k = 0; k < SUB; k++) {
      const ts = Math.min(t + ((k + 0.5) / SUB) * (SHUTTER / FPS), S0.end - 1e-6);
      sctx.globalCompositeOperation = 'source-over';
      sctx.globalAlpha = 1;
      sctx.drawImage(bgc, 0, 0);
      drawFg(sctx, ts, S0);
      watermarkIf(sctx, ts);
      actx.globalCompositeOperation = 'source-over';
      actx.globalAlpha = k === 0 ? 1 : 1 / (k + 1);
      actx.drawImage(scn, 0, 0);
    }
    actx.globalAlpha = 1;
    post(t, S0);
    if (Q.has('debug')) { const d = document.getElementById('debug'); d.hidden = false; d.textContent = `${t.toFixed(3)} s  ${S0.id}`; }
    return true;
  }
  function watermarkIf(c, t) {
    const S = sceneAt(t);
    if (S.id === 'open' || S.id === 'cta') return;
    watermark(c, t);
  }

  // Layout check: the boxes of the text drawn at time t with the camera held still.
  function layoutAt(t) {
    window.__layoutLog = [];
    window.__nocam = true;
    const sub = SUB;
    renderAtOnce(t);
    window.__nocam = false;
    const log = window.__layoutLog;
    window.__layoutLog = null;
    void sub;
    return log;
  }
  function renderAtOnce(t) {
    FT = t;
    const S0 = sceneAt(t);
    drawBg(bctx, t, S0);
    sctx.drawImage(bgc, 0, 0);
    drawFg(sctx, t, S0);
    watermarkIf(sctx, t);
  }

  window.renderAt = renderAt;
  window.layoutAt = layoutAt;
  window.promoInfo = () => ({ W, H, FORMAT, safe, SUB, cues: CUES && CUES.scenes });

  // ---------------------------------------------------------------- start up
  async function loadCues() {
    if (window.CUES) return window.CUES;
    const r = await fetch('cues.json');
    return r.json();
  }
  window.promoReady = (async () => {
    setCues(await loadCues());
    if (document.fonts && document.fonts.ready) await document.fonts.ready;
    if (!RENDER) {
      const fitCanvas = () => {
        const k = Math.min(innerWidth / W, innerHeight / H);
        out.style.width = `${W * k}px`;
        out.style.height = `${H * k}px`;
        out.style.left = `${(innerWidth - W * k) / 2}px`;
        out.style.top = `${(innerHeight - H * k) / 2}px`;
      };
      fitCanvas();
      addEventListener('resize', fitCanvas);
      if (Q.has('t')) {
        renderAt(Number(Q.get('t')));
      } else {
        let paused = false;
        let t0 = performance.now();
        let off = 0;
        const loop = (now) => {
          if (!paused) { lastT = (((now - t0) / 1000 + off) % DUR); renderAt(lastT); }
          requestAnimationFrame(loop);
        };
        requestAnimationFrame(loop);
        addEventListener('keydown', (e) => {
          if (e.code === 'Space') { paused = !paused; if (!paused) { t0 = performance.now(); off = lastT; } e.preventDefault(); }
          const step = e.shiftKey ? 1 : 1 / FPS;
          if (e.code === 'ArrowRight') { paused = true; renderAt((lastT + step) % DUR); }
          if (e.code === 'ArrowLeft') { paused = true; renderAt((lastT - step + DUR) % DUR); }
        });
      }
    }
  })();
})();
