// The session's numbers, in the seed's language: a rolling numeral, the frame-pacing strip drawn as
// a row of stems, and quiet readouts. Ported from the "precision instrument" study (instrument.ts:
// the odometer and the strip model are its logic) and restyled in styles/numbers.css.
//
// Cost: a roll is a translateY transition on each digit's reel (compositor only), the strip is drawn
// once per session and grows in once with a transform. Nothing here loops. While the pause switch is
// on (a game has the GPU) or under reduced motion the reels jump instead of rolling.

import { h, nextId, svg } from "../dom";
import { isPaused, reducedMotion } from "../motion";

// ---- Odometer ---------------------------------------------------------------------------------------

export interface Odometer {
  el: HTMLElement;
  /** Rolls every digit to the new text. Non-digits ("." "," "%" "–") are set flat. */
  set(text: string): void;
  value(): string;
}

/**
 * A rolling numeral. Each digit is a 0-9 reel moved by translateY, so a change is a compositor-only
 * roll. Screen readers get the plain text (the reels are aria-hidden), followed by `label`.
 * `lead`: leading zeros (and separators among them) are drawn faint, for a count padded to its total.
 */
export function odometer(text: string, opts: { cls?: string; label?: string; lead?: boolean } = {}): Odometer {
  const reels = h("span", { class: "odo-reels", "aria-hidden": "true" });
  const plain = h("span", { class: "sr-only" });
  const el = h("span", { class: `odo ${opts.cls ?? ""}` }, reels, plain);
  let current = "";

  const build = (t: string) => {
    // Rebuild only when the shape (digit or not, per position) changed; otherwise just roll.
    const shape = t.replace(/\d/g, "0");
    if (reels.dataset.shape === shape) return false;
    reels.dataset.shape = shape;
    const kids: HTMLElement[] = [];
    for (const ch of t) {
      if (/\d/.test(ch)) {
        const reel = h("span", { class: "odo-reel" }, Array.from({ length: 10 }, (_, n) => h("span", { text: String(n) })));
        kids.push(h("span", { class: "odo-d" }, reel));
      } else {
        kids.push(h("span", { class: "odo-s", text: ch }));
      }
    }
    reels.replaceChildren(...kids);
    return true;
  };

  const roll = (t: string) => {
    const cells = Array.from(reels.children) as HTMLElement[];
    const chars = Array.from(t);
    const n = cells.length;
    let leading = opts.lead === true;
    cells.forEach((cell, i) => {
      const ch = chars[i] ?? "";
      if (leading && /[1-9]/.test(ch)) leading = false;
      // The last digit is never faint: a count of 0 still reads "0".
      cell.classList.toggle("is-lead", leading && i < n - 1);
      if (cell.classList.contains("odo-d")) {
        const reel = cell.firstElementChild as HTMLElement;
        // Right-most digits lead, like a counter carrying over.
        reel.style.transitionDelay = `${(n - 1 - i) * 28}ms`;
        reel.style.transform = `translateY(${-Number(ch) * 10}%)`;
      } else {
        cell.textContent = ch;
      }
    });
  };

  const set = (t: string) => {
    if (t === current) return;
    current = t;
    plain.textContent = opts.label ? `${t} ${opts.label}` : t;
    // A fresh set of reels, a paused stage or reduced motion: jump, don't roll.
    const fresh = build(t);
    const still = fresh || reducedMotion() || isPaused();
    reels.classList.toggle("is-still", still);
    roll(t);
    if (still && fresh) requestAnimationFrame(() => reels.classList.remove("is-still"));
  };

  set(text);
  return { el, set, value: () => current };
}

/** "93.4" style: one decimal, or "–" when unknown. */
export function fixed1(n: number | null | undefined): string {
  return n === null || n === undefined || !Number.isFinite(n) ? "–" : n.toFixed(1);
}

const groupChar = (() => {
  try {
    return new Intl.NumberFormat().formatToParts(1000).find((p) => p.type === "group")?.value ?? ",";
  } catch {
    return ",";
  }
})();

/** `n` padded with zeros to as many digits as `total` has, grouped like the locale groups thousands. */
export function paddedCount(n: number, total: number): string {
  const digits = String(Math.max(0, Math.round(n))).padStart(String(Math.max(0, Math.round(total))).length, "0");
  let out = "";
  for (let i = 0; i < digits.length; i++) {
    const fromRight = digits.length - i;
    out += digits[i];
    if (fromRight > 1 && fromRight % 3 === 1) out += groupChar;
  }
  return out;
}

// ---- Frame-pacing strip -------------------------------------------------------------------------------

/** One finished session's frame pacing, as the emulator's log reports it (ShareOffer's numbers). */
export interface Pacing {
  at: string;
  game_fps: number;
  /** Percent of shown frames within 2 ms of the median interval: "frames on time". */
  within2: number;
  shown_fps: number;
  p50: number;
  p95: number;
  p99: number;
  gameplay_secs: number;
}

export interface PacingStrip {
  el: HTMLElement;
  set(p: Pacing): void;
  /** The stems grow in once (skipped under reduced motion). */
  reveal(delay?: number): void;
}

const N = 120;
const VW = 600;
const VH = 100;

/** Small seeded PRNG (mulberry32), so a title always draws the same profile. */
function rng(seed: string): () => number {
  let a = 2166136261;
  for (let i = 0; i < seed.length; i++) a = Math.imul(a ^ seed.charCodeAt(i), 16777619);
  return () => {
    a |= 0;
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/** A late stem is drawn at least this far over the median, so height (not only colour) marks it. */
const LATE_LIFT_MS = 6;

/**
 * Frame times modelled from the session's p50, p95 and p99 and its share of frames on time: a
 * profile captioned as such, never presented as a recording. Exactly round(N x late share) stems are
 * late, so the strip agrees with the numeral; the p99 and p95 tails are drawn only inside that share.
 */
function frames(p: Pacing, seed: string): { ms: number; late: boolean }[] {
  const r = rng(seed);
  const onTime = Math.min(1, Math.max(0, p.within2 / 100));
  const lateN = Math.round(N * (1 - onTime));
  const n99 = Math.min(lateN, Math.round(N * 0.01));
  const n95 = Math.min(lateN - n99, Math.round(N * 0.04));
  const floor = p.p50 + LATE_LIFT_MS;
  const out: { ms: number; late: boolean }[] = [];
  for (let i = 0; i < N; i++) {
    let ms: number;
    if (i < n99) ms = p.p99 * (0.92 + r() * 0.16);
    else if (i < n99 + n95) ms = p.p95 + Math.max(0, p.p99 - p.p95) * r() * 0.6;
    // The rest of the late share sits between the lift above the median and the p95.
    else if (i < lateN) ms = floor + Math.max(0, p.p95 - floor) * r();
    else ms = p.p50 + (r() - 0.5) * 3.2;
    const late = i < lateN;
    out.push({ ms: Math.max(1, late ? Math.max(ms, floor) : ms), late });
  }
  // Scatter the late frames through the session (seeded Fisher-Yates, so a title always looks the same).
  for (let i = out.length - 1; i > 0; i--) {
    const j = Math.floor(r() * (i + 1));
    [out[i], out[j]] = [out[j], out[i]];
  }
  return out;
}

/** The pacing strip: one stem per frame, on-time stems in leaf, late ones taller and in shell red. */
export function pacingStrip(opts: { seed: string; cls?: string }): PacingStrip {
  const id = nextId("strip");
  const graph = svg(
    `<svg class="strip-svg" viewBox="0 0 ${VW} ${VH}" preserveAspectRatio="none" aria-hidden="true" focusable="false">
      <defs><clipPath id="${id}-c"><rect x="0" y="0" width="${VW}" height="${VH}"/></clipPath></defs>
      <g clip-path="url(#${id}-c)">
        <rect class="strip-band" x="0" y="0" width="${VW}" height="0"/>
        <path class="strip-bars" d="" vector-effect="non-scaling-stroke"/>
        <path class="strip-late" d="" vector-effect="non-scaling-stroke"/>
        <path class="strip-median" d="" vector-effect="non-scaling-stroke"/>
      </g>
    </svg>`,
  );
  const q = <T extends Element>(s: string) => graph.querySelector<T>(s) as T;
  const band = q<SVGRectElement>(".strip-band");
  const bars = q<SVGPathElement>(".strip-bars");
  const late = q<SVGPathElement>(".strip-late");
  const median = q<SVGPathElement>(".strip-median");

  const axisMid = h("span", { class: "strip-axis" });
  const caption = h("figcaption", { class: "strip-caption" });
  const frame = h("div", { class: "strip-frame" }, graph, axisMid);
  const el = h("figure", { class: `strip ${opts.cls ?? ""}` }, frame, caption);

  const y = (ms: number, max: number) => VH - (Math.min(ms, max) / max) * (VH - 4);

  return {
    el,
    set(p) {
      // Scaled to the p95 so the body of the distribution has height; the rare p99 spikes run off
      // the top (clipped), which is what a spike looks like on a frame-time graph.
      const max = Math.max(p.p95 * 1.5, p.p50 * 2.5);
      const list = frames(p, opts.seed);
      const lo = p.p50 - 2;
      const hi = p.p50 + 2;
      let onD = "";
      let lateD = "";
      const step = VW / N;
      list.forEach(({ ms, late: isLate }, i) => {
        const x = (i * step + step / 2).toFixed(1);
        const seg = `M${x} ${VH}V${y(ms, max).toFixed(1)}`;
        if (isLate) lateD += seg;
        else onD += seg;
      });
      bars.setAttribute("d", onD);
      late.setAttribute("d", lateD);
      median.setAttribute("d", `M0 ${y(p.p50, max).toFixed(1)}H${VW}`);
      band.setAttribute("y", y(hi, max).toFixed(1));
      band.setAttribute("height", Math.max(1, y(lo, max) - y(hi, max)).toFixed(1));
      axisMid.textContent = `${p.p50.toFixed(1)} ms`;
      axisMid.style.setProperty("--y", `${((y(p.p50, max) / VH) * 100).toFixed(1)}%`);
      caption.textContent = "Modelled from the percentiles of the time between shown frames, not a recording. Late frames are red and taller.";
    },
    reveal(delay = 0) {
      if (reducedMotion()) return;
      el.style.setProperty("--grow-delay", `${delay}ms`);
      el.classList.remove("is-growing");
      void el.offsetWidth;
      el.classList.add("is-growing");
    },
  };
}

// ---- Readout ------------------------------------------------------------------------------------------

/** A measurement: the value (a numeral or text) with its unit, the label under it in plain words. */
export function readout(label: string, value: HTMLElement | string, opts: { unit?: string; note?: HTMLElement | null; cls?: string } = {}): HTMLElement {
  return h(
    "div",
    { class: `readout ${opts.cls ?? ""}` },
    h("span", { class: "readout-value" }, value, opts.unit ? h("span", { class: "readout-unit", text: opts.unit }) : null),
    h("span", { class: "readout-label", text: label }),
    opts.note ?? null,
  );
}
