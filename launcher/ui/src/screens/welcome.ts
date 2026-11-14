// First run. A ~2 s intro in a rainforest at night: one ember spark in the dark swells into a
// closed guarana fruit, its leaf unfurls, the shell splits and the fruit opens like an eye, looks
// around and settles its gaze on you. The name rises letter by letter, the echo arcs (predicted
// frames) slide out ahead of the fruit, then one button: "Set up", which the seed watches.
// Depth layers of canopy drift with the pointer; fireflies drift a few times and come to rest.
// Only transform and opacity move, except the leaf (a small SVG paint, once). Skippable with any
// key, click or pad button.

import { h, svg } from "../dom";
import { focusElement } from "../input";
import { reducedMotion } from "../motion";
import { go, preload, type ScreenDef } from "../router";
import { seed } from "../ui/seed";

/** A small seeded random, so the forest is the same forest every time. */
function rng(seedValue: number): () => number {
  let a = seedValue >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function leafPath(len: number, w: number): string {
  const f = (n: number) => n.toFixed(1);
  return `M0 0C${f(len * 0.28)} ${f(-w)} ${f(len * 0.74)} ${f(-w * 0.86)} ${f(len)} 0C${f(len * 0.74)} ${f(w * 0.86)} ${f(len * 0.28)} ${f(w)} 0 0Z`;
}

/** One depth layer of canopy: leaves reaching in from the left, the right and the top. */
function canopy(layer: number, fill: string, rim: string): SVGSVGElement {
  const r = rng(7 + layer * 31);
  const leaves: string[] = [];
  const count = 9 - layer * 2;
  for (let i = 0; i < count; i++) {
    const side = i % 3;
    const len = 260 + r() * 300 + layer * 80;
    const w = len * (0.16 + r() * 0.08);
    let x: number;
    let y: number;
    let a: number;
    if (side === 0) {
      x = -60 + r() * 120;
      y = 180 + r() * 820;
      a = -40 + r() * 60;
    } else if (side === 1) {
      x = 1660 - r() * 120;
      y = 120 + r() * 860;
      a = 180 + (-30 + r() * 60);
    } else {
      x = 200 + r() * 1200;
      y = -60 - r() * 40;
      a = 60 + r() * 60;
    }
    leaves.push(
      `<g transform="translate(${x.toFixed(0)} ${y.toFixed(0)}) rotate(${a.toFixed(1)})"><path d="${leafPath(len, w)}" fill="${fill}" stroke="${rim}" stroke-width="2"/><path d="M8 0H${(len * 0.92).toFixed(0)}" stroke="${rim}" stroke-width="2" fill="none"/></g>`,
    );
  }
  return svg(`<svg class="canopy" viewBox="0 0 1600 1000" preserveAspectRatio="xMidYMid slice" aria-hidden="true" focusable="false">${leaves.join("")}</svg>`);
}

const screen: ScreenDef = {
  title: "Welcome",
  chrome: "none",

  render(ctx) {
    preload("scan");
    const reduce = reducedMotion();
    const r = rng(42);

    const fireflies = Array.from({ length: 14 }, () =>
      h("span", { class: "firefly", style: { left: `${(6 + r() * 88).toFixed(1)}%`, top: `${(12 + r() * 76).toFixed(1)}%`, "--s": (0.5 + r() * 0.9).toFixed(2) } }),
    );
    const stage = h(
      "div",
      { class: "stage", "aria-hidden": "true" },
      h(
        "div",
        { class: "stage-dolly" },
        h("div", { class: "layer layer-far" }, canopy(0, "#03100a", "#0a2516")),
        h("div", { class: "layer layer-mid" }, h("div", { class: "haze haze-a ambient" }), canopy(1, "#051a0e", "#0d3020")),
        h("div", { class: "layer layer-near" }, canopy(2, "#020904", "#1a2a12"), fireflies),
      ),
      h("div", { class: "vignette" }),
    );

    const theSeed = seed({ size: "intro", state: reduce ? "open" : "closed", follow: true, breathe: true, echoes: true, cls: "intro-mark" });
    if (reduce) theSeed.ready(true);
    const spark = h("span", { class: "intro-spark" });
    const logo = h("div", { class: "intro-logo" }, h("div", { class: "bloom" }), theSeed.el, spark);
    const letters = Array.from("Guarana").map((c) => h("span", { class: "intro-letter", text: c }));
    const word = h("h1", { class: "intro-word", "aria-label": "Guarana" }, h("span", { class: "intro-word-text", "aria-hidden": "true" }, letters));
    const tag = h("p", { class: "intro-tag", text: "Switch 2 emulation, tuned for integrated GPUs." });
    const setUp = h(
      "button",
      { type: "button", class: "btn btn-primary btn-xl intro-cta", "data-primary": true, "data-autofocus": true },
      h("span", { class: "btn-label", text: "Set up" }),
    );
    const fine = h("p", { class: "intro-fine", text: "Not affiliated with Nintendo. No keys, firmware or games included." });
    const skip = h("button", { type: "button", class: "btn btn-ghost btn-sm intro-skip" }, h("span", { class: "btn-label", text: "Skip intro" }));
    const center = h("div", { class: "intro-center" }, logo, word, tag, setUp, fine);
    const el = h("div", { class: "intro", "data-phase": reduce ? "ready" : "intro" }, stage, center, skip);

    const anims: Animation[] = [];
    const timers: number[] = [];
    let phase: "intro" | "ready" = reduce ? "ready" : "intro";

    /** Ends the intro. A skip also lands the fireflies; the natural end lets them drift on. */
    const finish = (skipped = true) => {
      if (phase === "ready") return;
      phase = "ready";
      for (const t of timers) clearTimeout(t);
      for (const a of anims) {
        if (!skipped && a.playState === "finished") continue;
        if (!skipped && (a.effect?.getTiming().fill === "both")) continue;
        try {
          if (a.effect?.getTiming().iterations === Infinity) a.cancel();
          else a.finish();
        } catch {
          a.cancel();
        }
      }
      theSeed.setState("open");
      theSeed.ready(true);
      el.dataset.phase = "ready";
      skip.hidden = true;
      if (el.isConnected) focusElement(setUp);
    };

    // A click that skips the intro must not also press "Set up" underneath it.
    let skippedAt = 0;
    setUp.addEventListener("click", () => {
      if (phase !== "ready" || performance.now() - skippedAt < 400) return;
      void go("scan", { transition: "intro-exit" });
    });
    skip.addEventListener("click", () => {
      skippedAt = performance.now();
      finish();
    });
    ctx.onIntent((intent) => {
      if (phase === "intro") {
        finish();
        return true;
      }
      return intent === "back";
    });
    const onKey = (e: KeyboardEvent) => {
      if (phase === "intro" && !e.repeat) {
        e.preventDefault();
        finish();
      }
    };
    window.addEventListener("keydown", onKey, { capture: true, signal: ctx.signal });
    // Once "Set up" has finished appearing, pressing it works even while the last of the intro plays.
    let setUpVisible = false;
    el.addEventListener("pointerdown", (e) => {
      if (phase !== "intro") return;
      const onSetUp = e.target instanceof Node && setUp.contains(e.target);
      if (!(onSetUp && setUpVisible)) skippedAt = performance.now();
      finish();
    });

    // Depth: layers drift with the pointer (transform only, smoothed by CSS).
    let frame = 0;
    let mx = 0;
    let my = 0;
    el.addEventListener("pointermove", (e) => {
      if (reduce) return;
      mx = (e.clientX / innerWidth) * 2 - 1;
      my = (e.clientY / innerHeight) * 2 - 1;
      if (!frame)
        frame = requestAnimationFrame(() => {
          frame = 0;
          stage.style.setProperty("--mx", mx.toFixed(3));
          stage.style.setProperty("--my", my.toFixed(3));
        });
    });
    ctx.signal.addEventListener("abort", () => {
      for (const t of timers) clearTimeout(t);
    });

    if (reduce) {
      skip.hidden = true;
      return el;
    }

    // ---- Timeline (ms) ----
    const run = (target: Element | null, frames: Keyframe[], opts: KeyframeAnimationOptions): Animation | null => {
      if (!target) return null;
      const a = target.animate(frames, { fill: "backwards", ...opts });
      anims.push(a);
      return a;
    };
    const at = (ms: number, fn: () => void) => timers.push(window.setTimeout(() => phase === "intro" && fn(), ms));
    const jelly = "cubic-bezier(.3,1.6,.5,1)";
    const out = "cubic-bezier(.22,1,.36,1)";

    // The forest comes out of the dark, far layers first, and the camera eases back.
    run(stage, [{ opacity: 0 }, { opacity: 1 }], { duration: 900, easing: "ease-out" });
    run(stage.querySelector(".stage-dolly"), [{ transform: "scale(1.12)" }, { transform: "scale(1)" }], { duration: 3000, easing: out });
    for (const [i, layer] of Array.from(stage.querySelectorAll(".canopy")).entries()) {
      run(layer, [{ opacity: 0, transform: `translateY(${24 + i * 16}px)` }, { opacity: 1, transform: "none" }], { duration: 1300, delay: 100 + i * 140, easing: out });
    }
    run(stage.querySelector(".haze"), [{ opacity: 0, transform: "scale(.8)" }, { opacity: 1, transform: "none" }], { duration: 1600, delay: 300, easing: out });

    // One spark in the dark.
    run(spark, [
      { opacity: 0, transform: "scale(0)" },
      { opacity: 1, transform: "scale(1.2)", offset: 0.25 },
      { opacity: 1, transform: "scale(1)", offset: 0.55 },
      { opacity: 0, transform: "scale(5)" },
    ], { duration: 820, delay: 120, easing: out, fill: "both" });
    // It swells into the closed fruit.
    run(theSeed.el, [
      { opacity: 0, transform: "scale(.12) rotate(-30deg)" },
      { opacity: 1, transform: "none" },
    ], { duration: 820, delay: 420, easing: jelly });
    run(logo.querySelector(".bloom"), [
      { opacity: 0, transform: "scale(.3)" },
      { opacity: 1, transform: "scale(1.15)", offset: 0.5 },
      { opacity: 0.8, transform: "none" },
    ], { duration: 1500, delay: 420, easing: out });
    // The leaf unfurls.
    run(theSeed.el.querySelector(".seed-leaf"), [
      { opacity: 0, transform: "rotate(-70deg) scale(0)" },
      { opacity: 1, transform: "none" },
    ], { duration: 760, delay: 700, easing: jelly });
    // The shell splits: the fruit opens like an eye, looks up at you, and blinks once.
    at(980, () => theSeed.setState("open"));
    at(1350, () => theSeed.lookAt(innerWidth / 2, 0));
    at(1650, () => theSeed.lookAt(null));

    letters.forEach((l, i) =>
      run(l, [{ opacity: 0, transform: "translateY(.55em) rotate(8deg) scale(.9)" }, { opacity: 1, transform: "none" }], { duration: 640, delay: 1120 + i * 45, easing: jelly }),
    );
    run(tag, [{ opacity: 0, transform: "translateY(10px)" }, { opacity: 1, transform: "none" }], { duration: 600, delay: 1420, easing: out });
    // The echo arcs slide out ahead of the fruit: the predicted frames.
    at(1500, () => theSeed.ready(true));
    run(setUp, [{ opacity: 0, transform: "translateY(14px) scale(.9)" }, { opacity: 1, transform: "none" }], { duration: 640, delay: 1560, easing: jelly })
      ?.finished.then(() => (setUpVisible = true), () => undefined);
    // The fruit looks down at the button it wants you to press.
    at(1900, () => {
      const b = setUp.getBoundingClientRect();
      theSeed.lookAt(b.left + b.width / 2, b.top + b.height / 2);
    });
    run(fine, [{ opacity: 0 }, { opacity: 1 }], { duration: 600, delay: 1800, easing: "ease-out" });

    // The intro is "over" when the core timeline is; the fireflies finish in their own time.
    const core = [...anims];
    // Fireflies drift up once and come to rest, glowing faintly.
    fireflies.forEach((f, i) => {
      const dx = (r() - 0.5) * 60;
      const dy = -20 - r() * 50;
      run(f, [
        { opacity: 0, transform: "translate(0, 0) scale(.6)" },
        { opacity: 1, offset: 0.4 },
        { opacity: 0.45, transform: `translate(${dx.toFixed(0)}px, ${dy.toFixed(0)}px) scale(1)` },
      ], { duration: 2600 + r() * 1800, delay: 900 + i * 90, iterations: 1, easing: "ease-in-out", fill: "both", direction: "normal" });
    });

    void Promise.all(core.map((a) => a.finished.catch(() => undefined))).then(() => {
      if (!ctx.signal.aborted) finish(false);
    });
    return el;
  },
};

export default screen;
