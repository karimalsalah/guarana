#!/usr/bin/env node
// Renders the Guarana promo (site/promo/index.html) to video, one exact frame at a time.
//
// Each frame is set with window.renderAt(t) and captured as a PNG, so the result does not depend on
// how fast the machine is. The page draws real motion blur itself (4 sub-frames averaged per frame).
// score.mjs synthesizes the soundtrack from cues.json, the same cue list the picture uses. ffmpeg then
// writes, into --out:
//   guarana-promo-vertical.mp4      1080x1920, 60 fps, 20 s, H.264 High yuv420p (BT.709), CRF 18, AAC 192 kb/s
//   guarana-promo-landscape.mp4     1920x1080, same settings
//   guarana-promo-square.mp4        1080x1080, same settings
//   guarana-promo-vertical-15s.mp4  15 s cut of the vertical render for TikTok and Shorts (cues.json "cut15")
//   guarana-promo-teaser.mp4/.gif   6 s teaser of the landscape render that loops cut to cut ("teaser")
//   guarana-promo-thumbnail.jpg/.png  1280x720 YouTube thumbnail
//   guarana-promo-poster-<format>.png one still per format, for covers
//   guarana-score.wav (+ -15s, -teaser)  the soundtrack, 48 kHz stereo 16-bit
//
// Browser drivers:
//   playwright  used when the playwright (or playwright-core) package can be imported. CI installs
//               it with Chromium. Locally, --channel msedge (or chrome) drives an installed browser,
//               so no browser download is needed.
//   cdp         built in: starts an installed Edge or Chrome headless and drives it over the DevTools
//               protocol. Needs Node 22 or newer and nothing else. Used when Playwright is missing.
//
// Full local render on Windows (from the repository root), everything above in one go:
//   node site/promo/render.mjs --channel msedge --ffmpeg "C:\path\to\ffmpeg.exe" --out promo-out
// Quick look at a few frames without encoding:
//   node site/promo/render.mjs --format landscape --start 4 --end 4.2 --frames-only --keep-frames
// Layout check (clipped text, text outside the safe area, overlapping text), no video:
//   node site/promo/render.mjs --check-layout --channel msedge
//
// Options:
//   --format vertical,landscape,square|all   default all
//   --out <dir>                       default ./promo-out (keep it outside site/, which is published)
//   --fps <n>                         default 60
//   --sub <n>                         motion-blur sub-frames per frame, default 4 (1 turns it off)
//   --shutter <f>                     shutter in frames, default 0.75
//   --start <s> --end <s>             render part of the timeline (default 0 to 20; no extras then)
//   --driver auto|playwright|cdp      default auto
//   --channel chromium|msedge|chrome  default chromium with Playwright, msedge with the cdp driver
//   --browser-path <exe>              browser for the cdp driver (else PROMO_BROWSER, else a search)
//   --playwright-dir <dir>            where to resolve playwright from (else PLAYWRIGHT_DIR)
//   --ffmpeg <exe>                    default FFMPEG, else ffmpeg on PATH
//   --workers <n>                     browsers rendering in parallel (default: half the cores, at most 6)
//   --grain <n>                       luma grain strength added by ffmpeg to the MP4s, 0 for none (default 4)
//   --silent                          no score: every MP4 carries a silent stereo AAC track
//   --poster <s>                      poster time (default: "poster" in cues.json)
//   --no-thumb                        skip the thumbnail
//   --thumb-only                      render just the thumbnail
//   --keep-frames                     keep the PNG frames under <out>/frames
//   --frames-only                     capture frames, skip ffmpeg
//   --times <s,s,...>                 capture only the frames at these times (with --frames-only --keep-frames)
//   --timing                          print where the time per frame goes
//   --check-layout                    run the layout check instead of rendering

import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { copyFile, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import { availableParallelism, cpus, tmpdir } from 'node:os';
import { delimiter, dirname, extname, join, resolve, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const SITE = resolve(HERE, '..');
const FORMATS = { vertical: { width: 1080, height: 1920 }, landscape: { width: 1920, height: 1080 }, square: { width: 1080, height: 1080 } };
const THUMB = { width: 1280, height: 720 };

function parseArgs(argv) {
  const flags = new Set(['keep-frames', 'frames-only', 'timing', 'help', 'silent', 'no-thumb', 'check-layout', 'thumb-only']);
  const opts = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith('--')) throw new Error(`Unexpected argument: ${a}`);
    const [key, inline] = a.slice(2).split(/=(.*)/s, 2);
    if (flags.has(key)) opts[key] = true;
    else opts[key] = inline !== undefined ? inline : argv[++i];
    if (opts[key] === undefined) throw new Error(`--${key} needs a value`);
  }
  return opts;
}

const opts = parseArgs(process.argv.slice(2));
if (opts.help) {
  console.log('See the comment at the top of site/promo/render.mjs for options.');
  process.exit(0);
}
const cues = JSON.parse(await readFile(join(HERE, 'cues.json'), 'utf8'));
const DURATION = cues.duration;
const fps = Number(opts.fps || cues.fps || 60);
const start = Number(opts.start || 0);
const end = Math.min(Number(opts.end || DURATION), DURATION);
const fullRange = start === 0 && end === DURATION;
const outDir = resolve(opts.out || 'promo-out');
const formats = !opts.format || opts.format === 'all' ? Object.keys(FORMATS) : String(opts.format).split(',');
for (const f of formats) if (!FORMATS[f]) throw new Error(`Unknown format: ${f}`);
const posterAt = Number(opts.poster || cues.poster || 5.4);
const sub = Number(opts.sub || 4);
const shutter = Number(opts.shutter || 0.75);
const ffmpeg = opts.ffmpeg || process.env.FFMPEG || 'ffmpeg';
const grain = Number(opts.grain === undefined ? 4 : opts.grain);
const workers = Math.max(1, Number(opts.workers || Math.min(6, Math.floor((availableParallelism ? availableParallelism() : cpus().length) / 2))));

// ------------------------------------------------------------------ helpers
function run(cmd, args) {
  return new Promise((ok, fail) => {
    const p = spawn(cmd, args, { stdio: ['ignore', 'inherit', 'inherit'] });
    p.on('error', (e) => fail(new Error(`${cmd}: ${e.message}`)));
    p.on('exit', (code) => (code === 0 ? ok() : fail(new Error(`${cmd} exited with ${code}`))));
  });
}
const frameName = (i) => `f${String(i).padStart(5, '0')}.png`;
const timing = { set: 0, capture: 0, write: 0, frames: 0 };
// One expression (works with Playwright's evaluate and with Runtime.evaluate): set the frame, then
// wait for the compositor so the capture sees it painted.
const frameScript = (t) => `(async () => { window.renderAt(${JSON.stringify(t)}); await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))); return true; })()`;

// The page is served over http from site/ so it can fetch cues.json and share ../assets with the site.
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg', '.woff2': 'font/woff2' };
async function startServer() {
  const server = createServer(async (req, res) => {
    try {
      const url = new URL(req.url, 'http://x');
      const file = resolve(SITE, `.${decodeURIComponent(url.pathname)}`);
      if (file !== SITE && !file.startsWith(SITE + sep)) throw new Error('outside');
      const body = await readFile(file);
      res.writeHead(200, { 'content-type': MIME[extname(file)] || 'application/octet-stream', 'cache-control': 'no-store' });
      res.end(body);
    } catch {
      res.writeHead(404);
      res.end('not found');
    }
  });
  await new Promise((ok) => server.listen(0, '127.0.0.1', ok));
  return { server, base: `http://127.0.0.1:${server.address().port}` };
}

async function loadPlaywright() {
  const bases = [opts['playwright-dir'], process.env.PLAYWRIGHT_DIR, HERE, process.cwd()].filter(Boolean);
  for (const base of bases) {
    const req = createRequire(join(resolve(base), 'noop.js'));
    for (const name of ['playwright', 'playwright-core']) {
      try {
        const mod = await import(pathToFileURL(req.resolve(name)).href);
        const chromium = mod.chromium || (mod.default && mod.default.chromium);
        if (chromium) return { chromium, name, from: base };
      } catch {
        // not there; try the next place
      }
    }
  }
  return null;
}

function findBrowser(channel) {
  const fromEnv = opts['browser-path'] || process.env.PROMO_BROWSER;
  if (fromEnv) return fromEnv;
  const env = process.env;
  const list = [];
  if (process.platform === 'win32') {
    const roots = [env['ProgramFiles(x86)'], env.ProgramFiles, env.LOCALAPPDATA].filter(Boolean);
    const edge = roots.map((r) => join(r, 'Microsoft', 'Edge', 'Application', 'msedge.exe'));
    const chrome = roots.map((r) => join(r, 'Google', 'Chrome', 'Application', 'chrome.exe'));
    list.push(...(channel === 'chrome' ? [...chrome, ...edge] : [...edge, ...chrome]));
  } else if (process.platform === 'darwin') {
    const edge = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
    const chrome = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
    list.push(...(channel === 'chrome' ? [chrome, edge] : [edge, chrome]), '/Applications/Chromium.app/Contents/MacOS/Chromium');
  } else {
    const names = channel === 'chrome'
      ? ['google-chrome', 'google-chrome-stable', 'microsoft-edge', 'microsoft-edge-stable', 'chromium', 'chromium-browser']
      : ['microsoft-edge', 'microsoft-edge-stable', 'google-chrome', 'google-chrome-stable', 'chromium', 'chromium-browser'];
    for (const n of names) for (const dir of (env.PATH || '').split(delimiter)) list.push(join(dir, n));
  }
  return list.find((p) => existsSync(p));
}

// ------------------------------------------------------------------ driver: Playwright
async function playwrightDriver(pw, { width, height }) {
  const channel = opts.channel && opts.channel !== 'chromium' ? opts.channel : undefined;
  const browser = await pw.chromium.launch({
    channel,
    headless: true,
    args: ['--hide-scrollbars', '--force-color-profile=srgb', '--font-render-hinting=none', '--allow-file-access-from-files'],
  });
  const context = await browser.newContext({ viewport: { width, height }, deviceScaleFactor: 1, colorScheme: 'dark', reducedMotion: 'no-preference' });
  const page = await context.newPage();
  page.on('pageerror', (e) => console.error(`page error: ${e.message}`));
  page.on('console', (m) => {
    if (m.type() === 'error') console.error(`page console: ${m.text()}`);
  });
  return {
    name: `playwright (${pw.name} from ${pw.from}, ${channel || 'bundled chromium'})`,
    async open(url) {
      await page.goto(url, { waitUntil: 'load' });
      await page.evaluate('window.promoReady');
    },
    async eval(expression) {
      return page.evaluate(expression);
    },
    async frame(t, file) {
      const t0 = performance.now();
      await page.evaluate(frameScript(t));
      const t1 = performance.now();
      await page.screenshot({ path: file, type: 'png', clip: { x: 0, y: 0, width, height }, animations: 'allow', caret: 'initial', scale: 'css' });
      timing.set += t1 - t0;
      timing.capture += performance.now() - t1;
      timing.frames++;
    },
    async close() {
      await browser.close();
    },
  };
}

// ------------------------------------------------------------------ driver: built-in DevTools
async function cdpDriver({ width, height }) {
  if (typeof WebSocket !== 'function') throw new Error('The built-in driver needs Node 22 or newer (global WebSocket), or install Playwright.');
  const exe = findBrowser(opts.channel || 'msedge');
  if (!exe) throw new Error('No Edge or Chrome found. Pass --browser-path <exe>, or install Playwright.');
  const profile = await mkdtemp(join(tmpdir(), 'guarana-promo-'));
  const args = [
    '--headless=new', '--hide-scrollbars', '--mute-audio', '--no-first-run', '--no-default-browser-check',
    '--disable-extensions', '--disable-background-networking', '--disable-sync', '--disable-component-update',
    '--disable-features=Translate,MediaRouter', '--force-color-profile=srgb', '--force-device-scale-factor=1',
    '--disable-renderer-backgrounding', '--disable-background-timer-throttling', '--disable-backgrounding-occluded-windows',
    `--window-size=${width},${height}`, '--remote-debugging-port=0', `--user-data-dir=${profile}`, 'about:blank',
  ];
  const proc = spawn(exe, args, { stdio: ['ignore', 'ignore', 'pipe'] });
  const browserWs = await new Promise((ok, fail) => {
    let text = '';
    const timer = setTimeout(() => fail(new Error('The browser did not open a DevTools endpoint within 30 s')), 30000);
    proc.stderr.on('data', (d) => {
      text += d;
      const m = text.match(/DevTools listening on (ws:\/\/\S+)/);
      if (m) {
        clearTimeout(timer);
        ok(m[1]);
      }
    });
    proc.on('exit', (code) => {
      clearTimeout(timer);
      fail(new Error(`The browser exited (${code}) before DevTools was ready`));
    });
  });
  const host = new URL(browserWs).host;
  const targets = await (await fetch(`http://${host}/json/list`)).json();
  let target = targets.find((x) => x.type === 'page');
  if (!target) target = await (await fetch(`http://${host}/json/new?about:blank`, { method: 'PUT' })).json();

  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((ok, fail) => {
    ws.onopen = ok;
    ws.onerror = () => fail(new Error('Could not connect to the page over DevTools'));
  });
  let seq = 0;
  const pending = new Map();
  const listeners = [];
  ws.onmessage = (ev) => {
    const msg = JSON.parse(typeof ev.data === 'string' ? ev.data : Buffer.from(ev.data).toString('utf8'));
    if (msg.id !== undefined && pending.has(msg.id)) {
      const { ok, fail } = pending.get(msg.id);
      pending.delete(msg.id);
      if (msg.error) fail(new Error(msg.error.message));
      else ok(msg.result);
    } else if (msg.method) {
      if (msg.method === 'Runtime.exceptionThrown') {
        const d = msg.params.exceptionDetails;
        console.error(`page error: ${(d.exception && d.exception.description) || d.text}`);
      }
      for (const l of listeners.splice(0)) if (l.method !== msg.method) listeners.push(l);
      else l.ok(msg.params);
    }
  };
  const send = (method, params = {}) => new Promise((ok, fail) => {
    const id = ++seq;
    pending.set(id, { ok, fail });
    ws.send(JSON.stringify({ id, method, params }));
  });
  const once = (method) => new Promise((ok) => listeners.push({ method, ok }));
  const evaluate = async (expression) => {
    const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
    if (r.exceptionDetails) throw new Error((r.exceptionDetails.exception && r.exceptionDetails.exception.description) || r.exceptionDetails.text);
    return r.result && r.result.value;
  };
  await send('Page.enable');
  await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
  await send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: 'dark' }, { name: 'prefers-reduced-motion', value: 'no-preference' }] });

  return {
    name: `built-in DevTools driver (${exe})`,
    async open(url) {
      const loaded = once('Page.loadEventFired');
      await send('Page.navigate', { url });
      await loaded;
      await evaluate('window.promoReady');
    },
    eval: evaluate,
    async frame(t, file) {
      const t0 = performance.now();
      await evaluate(frameScript(t));
      const t1 = performance.now();
      const { data } = await send('Page.captureScreenshot', { format: 'png', clip: { x: 0, y: 0, width, height, scale: 1 }, captureBeyondViewport: false, optimizeForSpeed: true });
      const t2 = performance.now();
      await writeFile(file, Buffer.from(data, 'base64'));
      timing.set += t1 - t0;
      timing.capture += t2 - t1;
      timing.write += performance.now() - t2;
      timing.frames++;
    },
    async close() {
      try {
        ws.close();
      } catch {
        // already closed
      }
      proc.kill();
      await new Promise((r) => (proc.exitCode !== null ? r() : proc.once('exit', r)));
      for (let i = 0; i < 5; i++) {
        try {
          await rm(profile, { recursive: true, force: true });
          break;
        } catch {
          await new Promise((r) => setTimeout(r, 300));
        }
      }
    },
  };
}

// ------------------------------------------------------------------ encoding
const bt709 = 'colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv';
const grainFilter = grain > 0 ? `,noise=c0s=${grain}:c0f=t+u` : '';
const videoTail = (extra = '') => `scale=out_color_matrix=bt709:out_range=tv,format=yuv420p${grainFilter}${extra},setparams=${bt709}`;
const videoCodec = [
  '-c:v', 'libx264', '-preset', 'slow', '-crf', '18', '-profile:v', 'high', '-pix_fmt', 'yuv420p',
  '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709', '-color_range', 'tv', '-r', String(fps),
];
const audioCodec = ['-c:a', 'aac', '-b:a', '192k', '-ar', '48000', '-ac', '2'];
const silentInput = ['-f', 'lavfi', '-i', 'anullsrc=channel_layout=stereo:sample_rate=48000'];
const audioInput = (wav) => (wav ? ['-i', wav] : silentInput);
const seq = (framesDir, first, seconds) => ['-framerate', String(fps), '-start_number', String(first), '-t', seconds.toFixed(3), '-i', join(framesDir, 'f%05d.png')];

async function encodeMp4(framesDir, first, count, wav, file) {
  await run(ffmpeg, [
    '-hide_banner', '-loglevel', 'error', '-y',
    ...seq(framesDir, first, count / fps), ...(wav ? ['-ss', (first / fps).toFixed(3), '-i', wav] : silentInput),
    '-map', '0:v:0', '-map', '1:a:0', '-t', (count / fps).toFixed(3),
    '-vf', videoTail(), ...videoCodec, ...audioCodec, '-movflags', '+faststart', file,
  ]);
}
// Several stretches of the render joined cut to cut (the 15 s vertical cut).
async function encodeCut(framesDir, segments, wav, file) {
  const inputs = segments.flatMap(([a, b]) => seq(framesDir, Math.round(a * fps), b - a));
  const n = segments.length;
  const total = segments.reduce((s, [a, b]) => s + (b - a), 0);
  await run(ffmpeg, [
    '-hide_banner', '-loglevel', 'error', '-y',
    ...inputs, ...audioInput(wav),
    '-filter_complex', `${segments.map((_, i) => `[${i}:v]`).join('')}concat=n=${n}:v=1:a=0,${videoTail()}[v]`,
    '-map', '[v]', '-map', `${n}:a:0`, '-t', total.toFixed(3),
    ...videoCodec, ...audioCodec, '-movflags', '+faststart', file,
  ]);
}
async function encodeGif(framesDir, first, seconds, file) {
  await run(ffmpeg, [
    '-hide_banner', '-loglevel', 'error', '-y',
    ...seq(framesDir, first, seconds),
    '-vf', 'fps=15,scale=800:-2:flags=lanczos,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle',
    '-loop', '0', file,
  ]);
}

// ------------------------------------------------------------------ drivers and capture
async function makeDrivers(pw, size, count) {
  return Promise.all(Array.from({ length: count }, () => (pw ? playwrightDriver(pw, size) : cdpDriver(size))));
}
const pageUrl = (base, q) => `${base}/promo/index.html?${new URLSearchParams(q).toString()}`;

async function capture(drivers, url, framesDir, list, label) {
  const began = Date.now();
  timing.set = timing.capture = timing.write = timing.frames = 0;
  try {
    await Promise.all(drivers.map((d) => d.open(url)));
    let next = 0;
    let done = 0;
    await Promise.all(drivers.map(async (d) => {
      for (let k = next++; k < list.length; k = next++) {
        const i = list[k];
        await d.frame(i / fps, join(framesDir, frameName(i)));
        done++;
        if (done % fps === 0 || done === list.length) {
          process.stdout.write(`\r  ${label} ${done}/${list.length} frames, ${((Date.now() - began) / 1000).toFixed(0)} s`);
        }
      }
    }));
    process.stdout.write('\n');
    if (opts.timing && timing.frames) {
      const per = (v) => (v / timing.frames).toFixed(0);
      console.log(`  per frame and worker: set ${per(timing.set)} ms, capture ${per(timing.capture)} ms, write ${per(timing.write)} ms`);
    }
  } finally {
    await Promise.all(drivers.map((d) => d.close()));
  }
}

// ------------------------------------------------------------------ layout check
async function checkLayout(pw, base) {
  let bad = 0;
  for (const format of formats) {
    const size = FORMATS[format];
    const [d] = await makeDrivers(pw, size, 1);
    try {
      await d.open(pageUrl(base, { format, render: 1, sub: 1 }));
      const info = await d.eval('window.promoInfo()');
      const s = info.safe;
      const seen = new Map();
      let now = 0;
      // one line per distinct problem, with its first time and how many 0.05 s steps it lasts
      const report = (key, m) => { const e = seen.get(key); if (e) e.n++; else { seen.set(key, { m, n: 1, t: now }); bad++; } };
      for (let t = 0; t < DURATION; t += 0.05) {
        now = t;
        const log = await d.eval(`window.layoutAt(${t.toFixed(3)})`);
        for (const b of log) {
          if (b.x0 < s.x0 - 3 || b.x1 > s.x1 + 3 || b.y0 < s.y0 - 3 || b.y1 > s.y1 + 3) report(`safe|${b.grp}|${b.s}`, `"${b.s}" (${b.grp}) leaves the safe area: x ${b.x0.toFixed(0)}-${b.x1.toFixed(0)}, y ${b.y0.toFixed(0)}-${b.y1.toFixed(0)}`);
          if (b.x0 < -1 || b.x1 > size.width + 1 || b.y0 < -1 || b.y1 > size.height + 1) report(`clip|${b.grp}|${b.s}`, `"${b.s}" (${b.grp}) is clipped by the frame`);
        }
        for (let i = 0; i < log.length; i++) {
          for (let j = i + 1; j < log.length; j++) {
            const a = log[i];
            const b = log[j];
            if (a.grp === b.grp || a.free || b.free) continue;
            const w = Math.min(a.x1, b.x1) - Math.max(a.x0, b.x0);
            const h = Math.min(a.y1, b.y1) - Math.max(a.y0, b.y0);
            if (w > 3 && h > 3) report(`ov|${a.grp}|${b.grp}`, `"${a.s}" (${a.grp}) overlaps "${b.s}" (${b.grp}) by ${w.toFixed(0)}x${h.toFixed(0)} px`);
          }
        }
      }
      for (const e of seen.values()) console.log(`  ${format}: ${e.m}, from ${e.t.toFixed(2)} s for ${e.n} step(s)`);
    } finally {
      await d.close();
    }
  }
  console.log(bad ? `Layout check: ${bad} problem(s).` : 'Layout check: clean.');
  return bad;
}

// ------------------------------------------------------------------ main
async function main() {
  const wanted = opts.driver || 'auto';
  const pw = wanted === 'cdp' ? null : await loadPlaywright();
  if (wanted === 'playwright' && !pw) throw new Error('Playwright is not installed where this script can find it (see --playwright-dir).');
  const { server, base } = await startServer();
  try {
    if (opts['check-layout']) {
      process.exitCode = (await checkLayout(pw, base)) ? 1 : 0;
      return;
    }
    await mkdir(outDir, { recursive: true });
    const first = Math.round(start * fps);
    const stop = Math.round(end * fps); // exclusive: 20 s at 60 fps is frames 0..1199
    const outputs = [];
    // --times 1,2.5,4 captures just those frames (for looking at them); otherwise start..end
    const frameList = opts.times ? String(opts.times).split(',').map((x) => Math.round(Number(x) * fps)) : Array.from({ length: stop - first }, (_, k) => first + k);
    const encode = !opts['frames-only'];
    const extras = encode && fullRange;

    // The score, from the same cues.json as the picture.
    let wavs = { master: null, cut15: null, teaser: null };
    if (encode && !opts.silent) {
      const audioDir = join(outDir, 'audio');
      await mkdir(audioDir, { recursive: true });
      console.log('score: synthesizing');
      await run(process.execPath, [join(HERE, 'score.mjs'), '--out', audioDir]);
      wavs = { master: join(audioDir, 'guarana-score.wav'), cut15: join(audioDir, 'guarana-score-15s.wav'), teaser: join(audioDir, 'guarana-score-teaser.wav') };
      for (const w of Object.values(wavs)) outputs.push(w);
    }

    for (const format of opts['thumb-only'] ? [] : formats) {
      const { width, height } = FORMATS[format];
      const framesDir = join(outDir, 'frames', format);
      await rm(framesDir, { recursive: true, force: true });
      await mkdir(framesDir, { recursive: true });
      // Frames do not depend on each other, so several browsers render them side by side.
      const drivers = await makeDrivers(pw, { width, height }, workers);
      console.log(`${format}: ${width}x${height}, frames ${first}-${stop - 1} at ${fps} fps, ${sub} sub-frames, ${workers} worker(s), driver: ${drivers[0].name}`);
      await capture(drivers, pageUrl(base, { format, render: 1, sub, shutter }), framesDir, frameList, format);

      const posterFrame = Math.round(posterAt * fps);
      if (frameList.includes(posterFrame)) {
        const poster = join(outDir, `guarana-promo-poster-${format}.png`);
        await copyFile(join(framesDir, frameName(posterFrame)), poster);
        outputs.push(poster);
      }
      if (encode) {
        const mp4 = join(outDir, `guarana-promo-${format}.mp4`);
        await encodeMp4(framesDir, first, stop - first, wavs.master, mp4);
        outputs.push(mp4);
        if (extras && format === 'vertical') {
          const cut = join(outDir, 'guarana-promo-vertical-15s.mp4');
          await encodeCut(framesDir, cues.cut15, wavs.cut15, cut);
          outputs.push(cut);
        }
        if (extras && format === 'landscape') {
          const { start: a, end: b } = cues.teaser;
          const tm = join(outDir, 'guarana-promo-teaser.mp4');
          await run(ffmpeg, [
            '-hide_banner', '-loglevel', 'error', '-y',
            ...seq(framesDir, Math.round(a * fps), b - a), ...audioInput(wavs.teaser),
            '-map', '0:v:0', '-map', '1:a:0', '-t', (b - a).toFixed(3),
            '-vf', videoTail(), ...videoCodec, ...audioCodec, '-movflags', '+faststart', tm,
          ]);
          const gif = join(outDir, 'guarana-promo-teaser.gif');
          await encodeGif(framesDir, Math.round(a * fps), b - a, gif);
          outputs.push(tm, gif);
        }
      }
      if (!opts['keep-frames'] && encode) await rm(framesDir, { recursive: true, force: true });
    }

    // The 1280x720 YouTube thumbnail (a static composition of the same page).
    if ((extras || opts['thumb-only']) && !opts['no-thumb']) {
      const framesDir = join(outDir, 'frames', 'thumb');
      await mkdir(framesDir, { recursive: true });
      const drivers = await makeDrivers(pw, THUMB, 1);
      await capture(drivers, pageUrl(base, { w: THUMB.width, h: THUMB.height, mode: 'thumb', render: 1, sub: 1 }), framesDir, [0], 'thumbnail');
      const png = join(outDir, 'guarana-promo-thumbnail.png');
      await copyFile(join(framesDir, frameName(0)), png);
      await run(ffmpeg, ['-hide_banner', '-loglevel', 'error', '-y', '-i', png, '-q:v', '2', '-pix_fmt', 'yuvj444p', join(outDir, 'guarana-promo-thumbnail.jpg')]);
      outputs.push(png, join(outDir, 'guarana-promo-thumbnail.jpg'));
      if (!opts['keep-frames']) await rm(framesDir, { recursive: true, force: true });
    }
    if (!opts['keep-frames'] && encode) await rm(join(outDir, 'frames'), { recursive: true, force: true });
    console.log(outputs.length ? `Wrote:\n  ${outputs.join('\n  ')}` : `Frames are in ${join(outDir, 'frames')}`);
  } finally {
    server.close();
  }
}

main().catch((e) => {
  console.error(e.message || e);
  process.exit(1);
});
