// Motion policy: reduced motion, view transitions, idle work and the root pause switch.

const root = document.documentElement;
const reduceQuery = matchMedia("(prefers-reduced-motion: reduce)");
let forcedReduce = false;

export function initMotion(): void {
  const q = new URLSearchParams(location.search).get("motion");
  forcedReduce = q === "reduce";
  const sync = () => root.toggleAttribute("data-reduced", reducedMotion());
  sync();
  reduceQuery.addEventListener("change", sync);

  // Pause everything while the window is hidden; pause ambient loops while it is unfocused.
  document.addEventListener("visibilitychange", () => setPaused("hidden", document.hidden));
  window.addEventListener("blur", () => setIdle(true));
  window.addEventListener("focus", () => setIdle(false));
  setIdle(!document.hasFocus());
}

export function reducedMotion(): boolean {
  return forcedReduce || reduceQuery.matches;
}

// ---- Root pause switch -------------------------------------------------------------------------
// "game" (a game is running) and "hidden" pause every animation and loop: the launcher shares the
// GPU with the game. "Idle" (window unfocused) only pauses ambient decoration and gamepad polling.

const reasons = new Set<string>();
const pauseSubs = new Set<(paused: boolean) => void>();
let idle = false;
const idleSubs = new Set<(idle: boolean) => void>();

export function setPaused(reason: string, on: boolean): void {
  const before = reasons.size > 0;
  if (on) reasons.add(reason);
  else reasons.delete(reason);
  const after = reasons.size > 0;
  root.toggleAttribute("data-paused", after);
  if (before !== after) for (const fn of pauseSubs) fn(after);
}

export function isPaused(reason?: string): boolean {
  return reason ? reasons.has(reason) : reasons.size > 0;
}

export function onPauseChange(fn: (paused: boolean) => void): () => void {
  pauseSubs.add(fn);
  return () => pauseSubs.delete(fn);
}

function setIdle(on: boolean): void {
  if (idle === on) return;
  idle = on;
  root.toggleAttribute("data-idle", on);
  for (const fn of idleSubs) fn(on);
}

export function isIdle(): boolean {
  return idle;
}

/** Re-reads focus: a webview does not always get `blur` when its window loses focus. */
export function syncIdle(): boolean {
  setIdle(!document.hasFocus());
  return idle;
}

export function onIdleChange(fn: (idle: boolean) => void): () => void {
  idleSubs.add(fn);
  return () => idleSubs.delete(fn);
}

// ---- View transitions --------------------------------------------------------------------------

export type TransitionKind = "forward" | "back" | "tab-left" | "tab-right" | "fade" | "launch" | "return" | "intro-exit" | "theme" | "none";

interface ViewTransitionLike {
  finished: Promise<void>;
  ready: Promise<void>;
  updateCallbackDone: Promise<void>;
  skipTransition(): void;
}

type StartViewTransition = (update: () => void | Promise<void>) => ViewTransitionLike;

let transitionSeq = 0;
let running: ViewTransitionLike | null = null;

/** Runs `update` inside a view transition when possible; otherwise runs it directly. */
export async function viewTransition(update: () => void, kind: TransitionKind): Promise<void> {
  const doc = document as unknown as { startViewTransition?: StartViewTransition };
  if (kind === "none" || !doc.startViewTransition || reducedMotion() || document.hidden) {
    running?.skipTransition();
    update();
    return;
  }
  const seq = ++transitionSeq;
  root.dataset.vt = kind;
  try {
    const t = doc.startViewTransition(update);
    running = t;
    await t.updateCallbackDone;
    await t.finished.catch(() => undefined);
  } finally {
    if (seq === transitionSeq) {
      delete root.dataset.vt;
      running = null;
    }
  }
}

export function transitionRunning(): boolean {
  return running !== null;
}

// ---- Scheduling ----------------------------------------------------------------------------------

export function wait(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}

export function nextFrame(): Promise<number> {
  return new Promise((r) => requestAnimationFrame(r));
}

/** Runs `fn` when the main thread is idle (or after `timeout` ms). Returns a cancel function. */
export function whenIdle(fn: () => void, timeout = 2000): () => void {
  if (typeof requestIdleCallback === "function") {
    const id = requestIdleCallback(() => fn(), { timeout });
    return () => cancelIdleCallback(id);
  }
  const id = setTimeout(fn, 250);
  return () => clearTimeout(id);
}

/** Element.animate, skipped (jumps to the end state) under reduced motion. */
export function animate(el: Element, frames: Keyframe[], options: KeyframeAnimationOptions): Animation | null {
  if (reducedMotion() || typeof el.animate !== "function") return null;
  return el.animate(frames, options);
}
