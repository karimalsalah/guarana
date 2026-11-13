// First run. A ~3.4 s intro on a dark stage: the drop forms and stretches into motion, its streaks
// cut in, the leaf pops, the echo arcs (predicted frames) draw ahead, a light sweep crosses the
// logo, then one button: "Set up". Depth layers drift with the pointer; grain and bloom are static
// textures moved by transform/opacity only. Skippable with any key, click or pad button.

import { h, nextId, svg } from "../dom";
import { DROP_D, dropPath, echoD, LEAF_D } from "../icons";
import { focusElement } from "../input";
import { reducedMotion } from "../motion";
import { go, preload, type ScreenDef } from "../router";

let grainUrl: string | null = null;

/** A 160 px noise tile, generated once. */
function grain(): string {
  if (grainUrl) return grainUrl;
  const size = 160;
  const c = document.createElement("canvas");
  c.width = size;
  c.height = size;
  const g = c.getContext("2d");
  if (!g) return "";
  const img = g.createImageData(size, size);
  for (let i = 0; i < img.data.length; i += 4) {
    const v = Math.random() * 255;
    img.data[i] = v;
    img.data[i + 1] = v;
    img.data[i + 2] = v;
    img.data[i + 3] = Math.random() * 60;
  }
  g.putImageData(img, 0, 0);
  grainUrl = c.toDataURL("image/png");
  return grainUrl;
}

function introMark(): SVGSVGElement {
  const id = nextId("intro");
  const echoes = [0, 1, 2]
    .map((i) => `<path class="intro-echo intro-echo-${i}" d="${echoD(i * 7)}" pathLength="1" fill="none" stroke="#F59E0B" stroke-opacity="${[0.6, 0.3, 0.14][i]}" stroke-width="3" stroke-linecap="round"/>`)
    .join("");
  return svg(
    `<svg class="intro-mark" viewBox="-4 -2 86 68" aria-hidden="true" focusable="false">
      <defs>
        <linearGradient id="${id}-f" x1="22" y1="19" x2="48" y2="53" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="#FFC94A"/><stop offset="1" stop-color="#F2851A"/></linearGradient>
        <linearGradient id="${id}-sw" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#fff" stop-opacity="0"/><stop offset=".5" stop-color="#FFF6DC" stop-opacity=".85"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>
        <mask id="${id}-m" maskUnits="userSpaceOnUse" x="-10" y="-10" width="100" height="84">
          <rect x="-10" y="-10" width="100" height="84" fill="#fff"/>
          <path class="intro-cut" d="M29 29.2H-6" pathLength="1" stroke="#000" stroke-width="3.2" stroke-linecap="round"/>
          <path class="intro-cut" d="M29 42.8H-6" pathLength="1" stroke="#000" stroke-width="3.2" stroke-linecap="round"/>
        </mask>
        <clipPath id="${id}-c"><path d="${DROP_D}"/></clipPath>
      </defs>
      <g class="intro-drop-wrap">
        <path class="intro-drop" d="${DROP_D}" fill="url(#${id}-f)" mask="url(#${id}-m)"/>
        <g clip-path="url(#${id}-c)" mask="url(#${id}-m)"><rect class="intro-sweep" x="-34" y="-2" width="26" height="68" fill="url(#${id}-sw)" transform="skewX(-14)"/></g>
      </g>
      <path class="intro-leaf" d="${LEAF_D}" fill="#3CCF7E"/>
      ${echoes}
    </svg>`,
  );
}

const screen: ScreenDef = {
  title: "Welcome",
  chrome: "none",

  render(ctx) {
    preload("scan");
    const reduce = reducedMotion();

    const embers = Array.from({ length: 16 }, () => h("span", { class: "ember" }));
    const stage = h(
      "div",
      { class: "stage", "aria-hidden": "true" },
      h(
        "div",
        { class: "stage-dolly" },
        h("div", { class: "layer layer-far" }, h("div", { class: "stars ambient" })),
        h("div", { class: "layer layer-mid" }, h("div", { class: "haze haze-a ambient" }), h("div", { class: "haze haze-b ambient" })),
        h("div", { class: "layer layer-beam" }, h("div", { class: "beam" })),
        h("div", { class: "layer layer-near" }, embers),
      ),
      h("div", { class: "grain ambient" }),
      h("div", { class: "vignette" }),
    );
    const g = grain();
    if (g) stage.style.setProperty("--grain", `url("${g}")`);

    const mark = introMark();
    const logo = h("div", { class: "intro-logo" }, h("div", { class: "bloom" }), h("div", { class: "bloom bloom-wide" }), mark);
    const word = h("h1", { class: "intro-word" }, h("span", { class: "intro-word-text", text: "Guarana" }));
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
    let phase: "intro" | "ready" = reduce ? "ready" : "intro";

    const finish = () => {
      if (phase === "ready") return;
      phase = "ready";
      for (const a of anims) {
        try {
          if (a.effect?.getTiming().iterations === Infinity) a.cancel();
          else a.finish();
        } catch {
          a.cancel();
        }
      }
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
    const q = <T extends Element>(sel: string) => mark.querySelector<T>(sel);
    const spring = "cubic-bezier(.3,1.35,.5,1)";
    const out = "cubic-bezier(.22,1,.36,1)";

    run(stage, [{ opacity: 0 }, { opacity: 1 }], { duration: 800, easing: "ease-out" });
    run(stage.querySelector(".stage-dolly"), [{ transform: "scale(1.14)" }, { transform: "scale(1)" }], { duration: 3800, easing: out });
    run(stage.querySelector(".stars"), [{ opacity: 0 }, { opacity: 1 }], { duration: 1600, delay: 200, easing: "ease-out" });
    for (const [i, hz] of Array.from(stage.querySelectorAll(".haze")).entries()) {
      run(hz, [{ opacity: 0, transform: "scale(.8)" }, { opacity: 1, transform: "scale(1)" }], { duration: 1800, delay: 250 + i * 250, easing: out });
    }

    run(logo.querySelector(".bloom"), [
      { opacity: 0, transform: "scale(.4)" },
      { opacity: 1, transform: "scale(1.2)", offset: 0.55 },
      { opacity: 0.85, transform: "scale(1)" },
    ], { duration: 1800, delay: 300, easing: out });
    run(logo.querySelector(".bloom-wide"), [{ opacity: 0 }, { opacity: 1 }], { duration: 2200, delay: 1200, easing: "ease-out" });

    // The ball appears, then stretches into the drop (tail overshoots and settles).
    run(q(".intro-drop-wrap"), [{ opacity: 0, transform: "scale(.35)" }, { opacity: 1, transform: "scale(1)" }], { duration: 760, delay: 300, easing: spring });
    const drop = q<SVGPathElement>(".intro-drop");
    if (drop && CSS.supports("d", 'path("M0 0")')) {
      const tips = [18.6, 18.6, 14, 8, 3, 1, 2.6, 4];
      run(drop, tips.map((t, i) => ({ d: `path("${dropPath(t)}")`, offset: i / (tips.length - 1) })), { duration: 1150, delay: 650, easing: "cubic-bezier(.5,0,.3,1)" });
    }
    for (const [i, cut] of Array.from(mark.querySelectorAll(".intro-cut")).entries()) {
      run(cut, [{ strokeDasharray: "1 1", strokeDashoffset: 1 }, { strokeDasharray: "1 1", strokeDashoffset: 0 }], { duration: 620, delay: 1150 + i * 90, easing: out });
    }
    run(q(".intro-leaf"), [
      { opacity: 0, transform: "scale(0) rotate(-50deg)" },
      { opacity: 1, transform: "scale(1) rotate(0)" },
    ], { duration: 820, delay: 1500, easing: "cubic-bezier(.34,1.56,.64,1)" });
    for (const [i, arc] of Array.from(mark.querySelectorAll(".intro-echo")).entries()) {
      run(arc, [
        { strokeDasharray: "1 1", strokeDashoffset: 1, transform: "translateX(-6px)" },
        { strokeDasharray: "1 1", strokeDashoffset: 0, transform: "translateX(0)" },
      ], { duration: 560, delay: 1800 + i * 150, easing: out });
    }
    run(q(".intro-sweep"), [
      { transform: "translateX(0) skewX(-14deg)", opacity: 0 },
      { opacity: 1, offset: 0.2 },
      { transform: "translateX(118px) skewX(-14deg)", opacity: 0 },
    ], { duration: 1000, delay: 2150, easing: "cubic-bezier(.45,0,.25,1)", fill: "both" });
    run(stage.querySelector(".beam"), [
      { transform: "translateX(-70vw) rotate(14deg)", opacity: 0 },
      { opacity: 1, offset: 0.35 },
      { transform: "translateX(70vw) rotate(14deg)", opacity: 0 },
    ], { duration: 1700, delay: 1900, easing: "cubic-bezier(.45,0,.25,1)", fill: "both" });

    // Embers burst out behind the moving drop, then fade.
    embers.forEach((em, i) => {
      const angle = Math.PI * (0.62 + Math.random() * 0.76);
      const dist = 18 + Math.random() * 26;
      const dx = Math.cos(angle) * dist;
      const dy = Math.sin(angle) * dist * 0.55;
      const size = 0.6 + Math.random() * 0.9;
      run(em, [
        { opacity: 0, transform: `translate(0, 0) scale(${size})` },
        { opacity: 1, offset: 0.18 },
        { opacity: 0, transform: `translate(${dx.toFixed(1)}vw, ${dy.toFixed(1)}vh) scale(${(size * 0.4).toFixed(2)})` },
      ], { duration: 1500 + Math.random() * 1100, delay: 1050 + i * 28, easing: "cubic-bezier(.15,.7,.3,1)", fill: "both" });
    });

    run(word, [{ opacity: 0, transform: "translateY(16px)" }, { opacity: 1, transform: "translateY(0)" }], { duration: 760, delay: 2250, easing: spring });
    run(word.firstElementChild, [{ backgroundPosition: "100% 0" }, { backgroundPosition: "0% 0" }], { duration: 1100, delay: 2450, easing: "ease-in-out" });
    run(tag, [{ opacity: 0, transform: "translateY(10px)" }, { opacity: 1, transform: "translateY(0)" }], { duration: 640, delay: 2600, easing: out });
    run(setUp, [{ opacity: 0, transform: "translateY(12px) scale(.92)" }, { opacity: 1, transform: "translateY(0) scale(1)" }], { duration: 700, delay: 2900, easing: spring })
      ?.finished.then(() => (setUpVisible = true), () => undefined);
    run(fine, [{ opacity: 0 }, { opacity: 1 }], { duration: 600, delay: 3200, easing: "ease-out" });
    run(center, [{ transform: "scale(.97)" }, { transform: "scale(1)" }], { duration: 3600, easing: out });

    // Embers start at the drop's tail, wherever the layout put the logo.
    requestAnimationFrame(() => {
      const r = logo.getBoundingClientRect();
      stage.style.setProperty("--ox", `${Math.round(r.left + r.width * 0.3)}px`);
      stage.style.setProperty("--oy", `${Math.round(r.top + r.height * 0.56)}px`);
    });

    void Promise.all(anims.map((a) => a.finished.catch(() => undefined))).then(() => {
      if (!ctx.signal.aborted) finish();
    });
    return el;
  },
};

export default screen;
