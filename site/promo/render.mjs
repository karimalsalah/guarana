#!/usr/bin/env node
// Renders the Guarana promo (site/promo/index.html) to video, one exact frame at a time.
//
// Each frame is set with window.renderAt(t) and captured as a PNG, so the result does not depend
// on how fast the machine is. ffmpeg then writes, into --out:
//   guarana-promo-vertical.mp4    1080x1920, 30 fps, 20 s, H.264 yuv420p (BT.709), CRF 18, faststart
//   guarana-promo-landscape.mp4   1920x1080, same settings
//   guarana-promo-teaser.gif      6 s of the landscape render, 960 px wide, 15 fps
//   guarana-promo-poster-<format>.png  one still per format, for covers and thumbnails
// Music: none. Each MP4 carries a silent stereo AAC track, because some platforms expect audio.
//
// Browser drivers:
//   playwright  used when the playwright (or playwright-core) package can be imported. CI installs
//               it with Chromium. Locally, --channel msedge (or chrome) drives an installed browser,
//               so no browser download is needed.
//   cdp         built in: starts an installed Edge or Chrome headless and drives it over the DevTools
//               protocol. Needs Node 22 or newer and nothing else. Used when Playwright is missing.
//
// Local render on Windows (PowerShell, from the repository root):
//   node site/promo/render.mjs --channel msedge --ffmpeg "C:\path\to\ffmpeg.exe" --out promo-out
// Quick look at a few frames without encoding:
//   node site/promo/render.mjs --format landscape --start 4 --end 5 --frames-only --keep-frames
//
// Options:
//   --format vertical|landscape|all   default all
//   --out <dir>                       default ./promo-out (keep it outside site/, which is published)
//   --fps <n>                         default 30
//   --start <s> --end <s>             render part of the timeline (default 0 to 20)
//   --driver auto|playwright|cdp      default auto
//   --channel chromium|msedge|chrome  default chromium with Playwright, msedge with the cdp driver
//   --browser-path <exe>              browser for the cdp driver (else PROMO_BROWSER, else a search)
//   --playwright-dir <dir>            where to resolve playwright from (else PLAYWRIGHT_DIR)
//   --ffmpeg <exe>                    default FFMPEG, else ffmpeg on PATH
//   --workers <n>                     browsers rendering in parallel (default: half the cores, at most 4)
//   --grain <n>                       luma grain strength added by ffmpeg to the MP4s, 0 for none (default 4)
//   --gif-start <s> --gif-length <s>  teaser window, default 4.4 and 6
//   --poster <s>                      poster time, default 6.9
//   --keep-frames                     keep the PNG frames under <out>/frames
//   --frames-only                     capture frames, skip ffmpeg
//   --timing                          print where the time per frame goes

import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { copyFile, mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { availableParallelism, cpus, tmpdir } from 'node:os';
import { delimiter, dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const FORMATS = { vertical: { width: 1080, height: 1920 }, landscape: { width: 1920, height: 1080 } };
const DURATION = 20;

function parseArgs(argv) {
  const flags = new Set(['keep-frames', 'frames-only', 'timing', 'help']);
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
const fps = Number(opts.fps || 30);
const start = Number(opts.start || 0);
const end = Math.min(Number(opts.end || DURATION), DURATION);
const outDir = resolve(opts.out || 'promo-out');
const formats = !opts.format || opts.format === 'all' ? Object.keys(FORMATS) : String(opts.format).split(',');
for (const f of formats) if (!FORMATS[f]) throw new Error(`Unknown format: ${f}`);
const gifStart = Number(opts['gif-start'] || 4.4);
const gifLength = Number(opts['gif-length'] || 6);
const posterAt = Number(opts.poster || 6.9);
const ffmpeg = opts.ffmpeg || process.env.FFMPEG || 'ffmpeg';
const grain = Number(opts.grain === undefined ? 4 : opts.grain);
const workers = Math.max(1, Number(opts.workers || Math.min(4, Math.floor((availableParallelism ? availableParallelism() : cpus().length) / 2))));
const pageFile = join(HERE, 'index.html');

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
const pageUrl = (format) => `${pathToFileURL(pageFile).href}?format=${format}&render=1`;
// One expression (works with Playwright's evaluate and with Runtime.evaluate): set the frame, then
// wait two animation frames so the capture sees it painted.
const frameScript = (t) => `(async () => { window.renderAt(${JSON.stringify(t)}); await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))); return true; })()`;

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
    args: ['--hide-scrollbars', '--force-color-profile=srgb', '--font-render-hinting=none'],
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
async function encodeMp4(framesDir, first, count, file) {
  await run(ffmpeg, [
    '-hide_banner', '-loglevel', 'error', '-y',
    '-framerate', String(fps), '-start_number', String(first), '-i', join(framesDir, 'f%05d.png'),
    '-f', 'lavfi', '-i', 'anullsrc=channel_layout=stereo:sample_rate=48000',
    '-map', '0:v:0', '-map', '1:a:0', '-frames:v', String(count), '-t', (count / fps).toFixed(3),
    // A light luma grain, added here rather than in the page: it dithers the dark gradients against
    // banding, and keeps the PNG frames (and the GIF) clean and quick to capture.
    // setparams tags the frames as BT.709 so players read the colours as intended.
    '-vf', `scale=out_color_matrix=bt709:out_range=tv,format=yuv420p${grain > 0 ? `,noise=c0s=${grain}:c0f=t+u` : ''},setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv`,
    '-c:v', 'libx264', '-preset', 'slow', '-crf', '18', '-profile:v', 'high', '-pix_fmt', 'yuv420p',
    '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709', '-color_range', 'tv',
    '-r', String(fps), '-c:a', 'aac', '-b:a', '128k', '-movflags', '+faststart', file,
  ]);
}
async function encodeGif(framesDir, first, count, file) {
  await run(ffmpeg, [
    '-hide_banner', '-loglevel', 'error', '-y',
    '-framerate', String(fps), '-start_number', String(first), '-i', join(framesDir, 'f%05d.png'),
    '-t', (count / fps).toFixed(3),
    '-vf', 'fps=15,scale=960:-2:flags=lanczos,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle',
    '-loop', '0', file,
  ]);
}

// ------------------------------------------------------------------ main
async function main() {
  const wanted = opts.driver || 'auto';
  const pw = wanted === 'cdp' ? null : await loadPlaywright();
  if (wanted === 'playwright' && !pw) throw new Error('Playwright is not installed where this script can find it (see --playwright-dir).');
  await mkdir(outDir, { recursive: true });
  const first = Math.round(start * fps);
  const stop = Math.round(end * fps); // exclusive: 20 s at 30 fps is frames 0..599
  const outputs = [];

  for (const format of formats) {
    const { width, height } = FORMATS[format];
    const framesDir = join(outDir, 'frames', format);
    await rm(framesDir, { recursive: true, force: true });
    await mkdir(framesDir, { recursive: true });
    // Frames do not depend on each other, so several browsers render them side by side.
    const drivers = await Promise.all(Array.from({ length: workers }, () => (pw ? playwrightDriver(pw, { width, height }) : cdpDriver({ width, height }))));
    console.log(`${format}: ${width}x${height}, frames ${first}-${stop - 1} at ${fps} fps, ${workers} worker(s), driver: ${drivers[0].name}`);
    const began = Date.now();
    timing.set = timing.capture = timing.write = timing.frames = 0;
    try {
      await Promise.all(drivers.map((d) => d.open(pageUrl(format))));
      let next = first;
      let done = 0;
      await Promise.all(drivers.map(async (d) => {
        for (let i = next++; i < stop; i = next++) {
          await d.frame(i / fps, join(framesDir, frameName(i)));
          done++;
          if (done % fps === 0 || done === stop - first) {
            process.stdout.write(`\r  ${done}/${stop - first} frames, ${((Date.now() - began) / 1000).toFixed(0)} s`);
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

    const posterFrame = Math.round(posterAt * fps);
    if (posterFrame >= first && posterFrame < stop) {
      const poster = join(outDir, `guarana-promo-poster-${format}.png`);
      await copyFile(join(framesDir, frameName(posterFrame)), poster);
      outputs.push(poster);
    }
    if (!opts['frames-only']) {
      const mp4 = join(outDir, `guarana-promo-${format}.mp4`);
      await encodeMp4(framesDir, first, stop - first, mp4);
      outputs.push(mp4);
      const gifFirst = Math.round(gifStart * fps);
      const gifCount = Math.round(gifLength * fps);
      const teaserFormat = formats.includes('landscape') ? 'landscape' : formats[0];
      if (format === teaserFormat && gifFirst >= first && gifFirst + gifCount <= stop) {
        const gif = join(outDir, 'guarana-promo-teaser.gif');
        await encodeGif(framesDir, gifFirst, gifCount, gif);
        outputs.push(gif);
      }
    }
    if (!opts['keep-frames'] && !opts['frames-only']) await rm(framesDir, { recursive: true, force: true });
  }
  if (!opts['keep-frames'] && !opts['frames-only']) await rm(join(outDir, 'frames'), { recursive: true, force: true });
  console.log(outputs.length ? `Wrote:\n  ${outputs.join('\n  ')}` : `Frames are in ${join(outDir, 'frames')}`);
}

main().catch((e) => {
  console.error(e.message || e);
  process.exit(1);
});
