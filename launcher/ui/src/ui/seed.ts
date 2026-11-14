// The seed: a ripe guarana fruit split open, which looks like an eye. A red shell (the brand drop,
// with its speed streaks and leaf), white flesh, a black seed. It is the heart of the interface:
// it breathes while you are there, turns toward whatever has focus, dozes when nobody is around,
// and is the launch itself (launch.ts adds rings and orbiting sparks with `orbit()`).
//
// Cost: every moving part is its own HTML layer moved by transform or opacity, so the compositor
// does the work. Gaze and state changes are transitions, fired by events (focus, pointer, boot), never
// by a loop. The only loop is the breath on the big seed: stepped at 24 fps, on `.ambient` (stopped
// while the window is unfocused), stopped by html[data-paused], and stopped entirely once the seed
// falls asleep after a while without input. The rare blink is not a loop: a timer starts one short
// blink about every ten seconds, and only while the window has focus, nothing is paused and the seed
// is awake; idle, paused or asleep, the timer is not even set.
//
// The session ring: Home hands the seed the last finished session's share of frames on time, and the
// seed wears it as one leaf-green arc around the fruit, as long as that share (session()).

import { h, nextId, svg } from "../dom";
import { LEAF_D } from "../icons";
import { inputMode, onInputMode } from "../input";
import { isIdle, isPaused, onIdleChange, onPauseChange, reducedMotion, syncIdle } from "../motion";

export type SeedSize = "mark" | "stage" | "launch" | "intro";
/** closed: a whole fruit (a seam). drowsy: half open. open: awake. wide: the iris opening. shut: failed. */
export type SeedState = "closed" | "drowsy" | "open" | "wide" | "shut";

export interface Seed {
  el: HTMLElement;
  /** The eye: its rect is what other layers (the launch void) align to. */
  pupil: HTMLElement;
  setState(s: SeedState): void;
  state(): SeedState;
  /** Looks at a point in client coordinates; null looks straight ahead. */
  lookAt(x: number | null, y?: number): void;
  /** Pupil size: 1 is rest, above 1 dilates (interest, launch), below contracts. */
  dilate(scale: number): void;
  /** The echo arcs (predicted frames) slide out ahead of the seed, or back in. */
  ready(on: boolean): void;
  /** The session ring: the share (0 to 1) of the last session's frames on time; null removes it. */
  session(onTime: number | null): void;
}

// Geometry in a 200 x 200 box: the brand drop (circle centre 112,100, r 62, tail tip at x 14), the
// eye centred in the circle. The CSS places the eye layers with the same numbers (seed.css).
const CX = 112;
const CY = 100;
const R = 62;
const TIP = 14;

function dropD(): string {
  const d = CX - TIP;
  const cos = R / d;
  const sin = Math.sqrt(1 - cos * cos);
  const x = (CX - R * cos).toFixed(2);
  const yTop = (CY - R * sin).toFixed(2);
  const yBot = (CY + R * sin).toFixed(2);
  return `M${TIP} ${CY}L${x} ${yTop}A${R} ${R} 0 1 1 ${x} ${yBot}Z`;
}

/** The brand leaf, scaled from the 64 px mark onto the top of the fruit. */
function leafTransform(): string {
  const s = 3.1;
  // The leaf's base (37.5, 20) in the mark sits at about (0.09 r, -0.94 r) from the circle centre.
  const bx = CX + 0.09 * R;
  const by = CY - 0.94 * R;
  return `translate(${(bx - 37.5 * s).toFixed(2)} ${(by - 20 * s).toFixed(2)}) scale(${s})`;
}

function shellSvg(): SVGSVGElement {
  const id = nextId("seed");
  const streakY = [CY - 0.4 * R, CY + 0.4 * R];
  const streaks = streakY.map((y) => `<path d="M-6 ${y.toFixed(1)}H72" stroke="#000" stroke-width="6.6" stroke-linecap="round"/>`).join("");
  const rim = `M${(CX + R * Math.cos(-1.25)).toFixed(1)} ${(CY + R * Math.sin(-1.25)).toFixed(1)}A${R - 3} ${R - 3} 0 0 1 ${(CX + R * Math.cos(-0.2)).toFixed(1)} ${(CY + R * Math.sin(-0.2)).toFixed(1)}`;
  return svg(
    `<svg class="seed-shell" viewBox="0 0 200 200" aria-hidden="true" focusable="false">
      <defs>
        <radialGradient id="${id}-s" cx="0.66" cy="0.3" r="0.8">
          <stop offset="0" stop-color="#ff9a52"/><stop offset="0.3" stop-color="#e64422"/><stop offset="0.68" stop-color="#a8190f"/><stop offset="1" stop-color="#4e0906"/>
        </radialGradient>
        <linearGradient id="${id}-t" x1="${TIP}" y1="0" x2="${CX}" y2="0" gradientUnits="userSpaceOnUse">
          <stop offset="0" stop-color="#ffc94a"/><stop offset="0.45" stop-color="#f2851a"/><stop offset="1" stop-color="#f2851a" stop-opacity="0"/>
        </linearGradient>
        <linearGradient id="${id}-fade" x1="${TIP}" y1="0" x2="${CX - R * 0.55}" y2="0" gradientUnits="userSpaceOnUse">
          <stop offset="0" stop-color="#fff" stop-opacity="0.08"/><stop offset="1" stop-color="#fff"/>
        </linearGradient>
        <mask id="${id}-m" maskUnits="userSpaceOnUse" x="-10" y="-10" width="220" height="220">
          <rect x="-10" y="-10" width="220" height="220" fill="url(#${id}-fade)"/>${streaks}
        </mask>
      </defs>
      <g mask="url(#${id}-m)">
        <path d="${dropD()}" fill="url(#${id}-s)"/>
        <path d="${dropD()}" fill="url(#${id}-t)" opacity="0.55"/>
      </g>
      <path d="${rim}" fill="none" stroke="#ffd2a8" stroke-opacity="0.38" stroke-width="2.2" stroke-linecap="round"/>
      <g class="seed-leaf"><g transform="${leafTransform()}"><path d="${LEAF_D}" fill="#3CCF7E"/><path d="M37.9 19.4Q41.5 12.8 47.2 9.8" fill="none" stroke="#1d8f52" stroke-width="0.5" stroke-linecap="round"/></g></g>
    </svg>`,
  );
}

function echoSvg(i: number): SVGSVGElement {
  // The brand's echo arc: the drop's circle shifted ahead, only its leading edge drawn.
  const dx = 26 + i * 24;
  const a = 0.72;
  const x = (CX + dx + R * Math.cos(-a)).toFixed(1);
  const y1 = (CY + R * Math.sin(-a)).toFixed(1);
  const y2 = (CY + R * Math.sin(a)).toFixed(1);
  return svg(
    `<svg class="seed-echo seed-echo-${i}" viewBox="0 0 200 200" aria-hidden="true" focusable="false"><path d="M${x} ${y1}A${R} ${R} 0 0 1 ${x} ${y2}" fill="none" stroke="#F59E0B" stroke-width="5" stroke-linecap="round"/></svg>`,
  );
}

/** The session ring: a track and an arc around the fruit, where the launch's first ring blooms. */
function sessionRingSvg(): SVGSVGElement {
  // Radius 0.415 of the box: the launch orbit's first ring (k 1.34 of the fruit's 62).
  const r = 83;
  return svg(
    `<svg class="seed-ring" viewBox="0 0 200 200" aria-hidden="true" focusable="false">
      <circle class="seed-ring-track" cx="${CX}" cy="${CY}" r="${r}" fill="none"/>
      <circle class="seed-ring-arc" cx="${CX}" cy="${CY}" r="${r}" fill="none" pathLength="100" transform="rotate(-90 ${CX} ${CY})"/>
    </svg>`,
  );
}

// ---- Gaze and attention: one set of listeners for every live seed ---------------------------------

interface Live {
  el: HTMLElement;
  look(x: number | null, y?: number): void;
  sleep(on: boolean): void;
}

const live = new Set<Live>();
let wired = false;
let asleep = false;
let sleepTimer = 0;
let pointerFrame = 0;
let px = 0;
let py = 0;
/** Without any input for this long the seed dozes off: half-closed, no breath, nothing moves. */
const SLEEP_MS = 45_000;

function eachLive(fn: (s: Live) => void): void {
  for (const s of live) {
    if (!s.el.isConnected) live.delete(s);
    else fn(s);
  }
}

function lookAtFocus(): void {
  const el = document.activeElement;
  if (!(el instanceof HTMLElement) || el === document.body || inputMode() === "mouse") return;
  const r = el.getBoundingClientRect();
  if (r.width < 1) return;
  eachLive((s) => s.look(r.left + r.width / 2, r.top + r.height / 2));
}

function wake(): void {
  if (asleep) {
    asleep = false;
    eachLive((s) => s.sleep(false));
  }
  clearTimeout(sleepTimer);
  sleepTimer = window.setTimeout(() => {
    if (isPaused()) return;
    asleep = true;
    stopBlinks();
    eachLive((s) => {
      s.look(null);
      s.sleep(true);
    });
  }, SLEEP_MS);
  if (!blinkTimer) scheduleBlink(false);
}

// ---- Blink: one short blink at a time, started by a timer that only runs while someone is there -----

interface Blinker {
  el: HTMLElement;
  lid: HTMLElement;
}

const blinkers = new Set<Blinker>();
let blinkTimer = 0;
let blinkWired = false;
/** About this long between blinks (with a little variation, so it never reads as a metronome). */
const BLINK_MS = 10_400;

function stopBlinks(): void {
  clearTimeout(blinkTimer);
  blinkTimer = 0;
}

function blinkAllowed(): boolean {
  return blinkers.size > 0 && !asleep && !isPaused() && !isIdle() && !document.hidden && !reducedMotion();
}

function scheduleBlink(first: boolean): void {
  stopBlinks();
  if (!blinkAllowed()) return;
  blinkTimer = window.setTimeout(blinkNow, first ? 2500 : BLINK_MS * (0.8 + Math.random() * 0.4));
}

function blinkNow(): void {
  blinkTimer = 0;
  // Focus is read again here: a webview does not always get `blur` when its window loses focus.
  // Unfocused, the timer stays off until focus, input or the pause switch brings it back.
  syncIdle();
  if (!blinkAllowed()) return;
  for (const b of blinkers) {
    if (!b.el.isConnected) {
      blinkers.delete(b);
      continue;
    }
    if (b.el.dataset.state !== "open" || b.el.hasAttribute("data-sleep") || typeof b.lid.animate !== "function") continue;
    // About 300 ms, stepped to 24 fps like the breath: close in four steps, open in five.
    b.lid.animate(
      [
        { transform: "scaleY(1)", easing: "steps(4, jump-end)" },
        { transform: "scaleY(0.06)", offset: 0.43, easing: "steps(5, jump-end)" },
        { transform: "scaleY(1)" },
      ],
      { duration: 312 },
    );
  }
  scheduleBlink(false);
}

function wireBlink(): void {
  if (blinkWired) return;
  blinkWired = true;
  onIdleChange((idle) => (idle ? stopBlinks() : scheduleBlink(false)));
  onPauseChange((paused) => (paused ? stopBlinks() : scheduleBlink(false)));
  document.addEventListener("visibilitychange", () => (document.hidden ? stopBlinks() : scheduleBlink(false)));
}

function wire(): void {
  if (wired) return;
  wired = true;
  document.addEventListener("focusin", () => {
    wake();
    lookAtFocus();
  });
  document.addEventListener("keydown", wake, { passive: true });
  document.addEventListener("pointerdown", wake, { passive: true });
  document.addEventListener("wheel", wake, { passive: true });
  document.addEventListener(
    "pointermove",
    (e) => {
      if (inputMode() !== "mouse" || e.pointerType !== "mouse") return;
      px = e.clientX;
      py = e.clientY;
      if (pointerFrame) return;
      pointerFrame = requestAnimationFrame(() => {
        pointerFrame = 0;
        wake();
        eachLive((s) => s.look(px, py));
      });
    },
    { passive: true },
  );
  document.documentElement.addEventListener("pointerleave", () => eachLive((s) => s.look(null)));
  onInputMode((m) => {
    if (m === "mouse") eachLive((s) => s.look(null));
    else lookAtFocus();
  });
  wake();
}

// ---- The seed ----------------------------------------------------------------------------------------

export function seed(opts: { size: SeedSize; state?: SeedState; follow?: boolean; breathe?: boolean; echoes?: boolean; cls?: string }): Seed {
  const glint = h("span", { class: "seed-glint" });
  const pupil = h("span", { class: "seed-pupil" });
  const iris = h("span", { class: "seed-iris" }, pupil, glint, h("span", { class: "seed-glint seed-glint-2" }));
  const eye = h("span", { class: "seed-eye" }, iris);
  const lid = h("span", { class: "seed-lid" }, eye);
  const body = h("span", { class: `seed-body${opts.breathe ? " ambient" : ""}` }, shellSvg(), lid);
  const echoes = opts.echoes ? h("span", { class: "seed-echoes" }, [0, 1, 2].map(echoSvg)) : null;
  const el = h(
    "div",
    {
      class: `seed seed-${opts.size}${opts.breathe ? " is-breathing" : ""} ${opts.cls ?? ""}`,
      "data-state": opts.state ?? "open",
      "aria-hidden": "true",
    },
    h("span", { class: `seed-aura${opts.breathe ? " ambient" : ""}` }),
    echoes,
    body,
  );

  let current: SeedState = opts.state ?? "open";
  let dilation = 1;
  let gx = 0;
  let gy = 0;

  const applyIris = () => {
    iris.style.transform = gx || gy ? `translate(${gx.toFixed(1)}px, ${gy.toFixed(1)}px)` : "";
    // The highlight is a reflection: it barely follows the seed, which makes the seed read as round.
    glint.style.transform = gx || gy ? `translate(${(-gx * 0.35).toFixed(1)}px, ${(-gy * 0.35).toFixed(1)}px)` : "";
  };

  const look = (x: number | null, y = 0) => {
    if (x === null || current === "closed" || current === "shut") {
      gx = 0;
      gy = 0;
      applyIris();
      return;
    }
    const r = body.getBoundingClientRect();
    if (r.width < 2) return;
    const cx = r.left + r.width * (CX / 200);
    const cy = r.top + r.height * (CY / 200);
    const dx = x - cx;
    const dy = y - cy;
    const dist = Math.hypot(dx, dy);
    if (dist < r.width * 0.12) {
      gx = 0;
      gy = 0;
    } else {
      // Saturating: near things move the eye a little, far things to its edge.
      const k = dist / (dist + r.width * 0.9);
      gx = (dx / dist) * k * r.width * 0.085;
      gy = (dy / dist) * k * r.width * 0.045;
    }
    applyIris();
  };

  const api: Seed = {
    el,
    pupil,
    setState(s) {
      current = s;
      el.dataset.state = s;
      if (s === "closed" || s === "shut") look(null);
    },
    state: () => current,
    lookAt: look,
    dilate(scale) {
      dilation = scale;
      pupil.style.transform = dilation === 1 ? "" : `scale(${dilation.toFixed(3)})`;
    },
    ready(on) {
      el.toggleAttribute("data-ready", on);
    },
    session(onTime) {
      if (onTime === null || !Number.isFinite(onTime)) {
        ring?.remove();
        ring = null;
        return;
      }
      const share = Math.max(0, Math.min(1, onTime)) * 100;
      if (!ring) {
        ring = sessionRingSvg();
        el.insertBefore(ring, echoes ?? body);
      }
      const arc = ring.querySelector<SVGCircleElement>(".seed-ring-arc");
      if (!arc) return;
      // Drawn in from the top, clockwise: a one-off transition of the dash, nothing loops.
      void getComputedStyle(arc).strokeDasharray;
      arc.style.strokeDasharray = `${share.toFixed(2)} 100`;
    },
  };
  let ring: SVGSVGElement | null = null;
  if (opts.breathe) {
    wireBlink();
    blinkers.add({ el, lid });
    scheduleBlink(true);
  }

  if (opts.follow) {
    wire();
    live.add({
      el,
      look,
      sleep: (on) => {
        el.toggleAttribute("data-sleep", on);
      },
    });
    if (asleep) el.setAttribute("data-sleep", "");
    // Once it is on the page, look at whatever already has focus.
    requestAnimationFrame(() => {
      if (el.isConnected && inputMode() !== "mouse") lookAtFocus();
    });
  }
  return api;
}

// ---- Orbit: the boot phases as rings, shader counts as sparks settling into place ---------------------

export interface Orbit {
  el: HTMLElement;
  /** Ring i (0 starting, 1 loading, 2 shaders, 3 starting the game) becomes the live one; the rings inside it settle. */
  phase(i: number): void;
  /** Shader counts: `done` of `total` sparks have settled onto the shader ring. */
  particles(done: number, total: number): void;
  /** Everything falls into the seed: the iris is about to open. */
  release(): void;
  /** The launch failed: the rings go dark and still. */
  fail(): void;
}

const RINGS = [1.34, 1.6, 1.9, 2.22];
const TICKS = 64;
const MOTES = 14;

export function orbit(): Orbit {
  const rings = RINGS.map((k, i) =>
    h(
      "span",
      { class: `orbit-ring orbit-ring-${i}`, style: { "--k": String(k), "--spin": `${(2.6 + i * 0.8).toFixed(1)}s` } },
      h("span", { class: "orbit-spin" }, h("span", { class: "orbit-spark" })),
    ),
  );
  // Ticks sit on the shader ring; each lights when its share of the shaders is built.
  const ticks = Array.from({ length: TICKS }, (_, i) =>
    h("span", { class: "orbit-tick", style: { "--a": `${((i / TICKS) * 360 - 90).toFixed(2)}deg` } }),
  );
  const motes = Array.from({ length: MOTES }, () => {
    const a = Math.random() * 360;
    const k = RINGS[2] + 0.08 + Math.random() * (RINGS[3] - RINGS[2] - 0.12);
    return h("span", { class: "orbit-mote", style: { "--a": `${a.toFixed(1)}deg`, "--k": k.toFixed(3), "--s": (0.6 + Math.random() * 0.7).toFixed(2) } });
  });
  const tickRing = h("span", { class: "orbit-ticks", style: { "--k": String(RINGS[2]) } }, ticks);
  const swarm = h("span", { class: "orbit-swarm" }, motes);
  const el = h("div", { class: "orbit", "aria-hidden": "true" }, rings, tickRing, swarm);

  let lit = 0;
  let current = -1;

  const fly = (tickIndex: number) => {
    if (reducedMotion() || isPaused() || typeof el.animate !== "function") return;
    const tr = rings[2].getBoundingClientRect().width / 2;
    const fr = (rings[3].getBoundingClientRect().width / 2) * 0.96;
    if (tr < 4) return;
    const from = Math.random() * Math.PI * 2;
    const to = (((tickIndex / TICKS) * 360 - 90) * Math.PI) / 180;
    const mote = h("span", { class: "orbit-flight" });
    el.appendChild(mote);
    const a = mote.animate(
      [
        { transform: `translate(${(Math.cos(from) * fr).toFixed(1)}px, ${(Math.sin(from) * fr).toFixed(1)}px) scale(1)`, opacity: 0 },
        { opacity: 1, offset: 0.25 },
        { transform: `translate(${(Math.cos(to) * tr).toFixed(1)}px, ${(Math.sin(to) * tr).toFixed(1)}px) scale(0.6)`, opacity: 0.2 },
      ],
      { duration: 520 + Math.random() * 260, easing: "cubic-bezier(.3,.1,.2,1)", fill: "both" },
    );
    a.finished.then(() => mote.remove(), () => mote.remove());
  };

  return {
    el,
    phase(i) {
      if (i === current) return;
      current = i;
      rings.forEach((r, j) => {
        r.classList.toggle("is-live", j === i);
        r.classList.toggle("is-settled", j < i);
      });
      el.dataset.phase = String(i);
    },
    particles(done, total) {
      const f = total > 0 ? Math.max(0, Math.min(1, done / total)) : 0;
      const target = Math.round(f * TICKS);
      el.classList.add("is-counting");
      // Free sparks leave the swarm as they settle.
      const free = Math.ceil(MOTES * (1 - f));
      motes.forEach((m, i) => m.classList.toggle("is-gone", i >= free));
      if (target <= lit) return;
      const fresh = target - lit;
      for (let i = lit; i < target; i++) {
        const t = ticks[i];
        t.style.transitionDelay = `${Math.min(i - lit, 12) * 18}ms`;
        t.classList.add("is-lit");
      }
      // A few visible flights per update read as "settling"; more would only cost.
      const flights = Math.min(4, fresh);
      for (let n = 0; n < flights; n++) fly(lit + Math.floor(((n + 1) * fresh) / (flights + 1)));
      lit = target;
    },
    release() {
      el.classList.add("is-released");
    },
    fail() {
      el.classList.add("is-failed");
      rings.forEach((r) => r.classList.remove("is-live"));
    },
  };
}
