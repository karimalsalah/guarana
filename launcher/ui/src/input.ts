// One intent layer for keyboard and gamepad: spatial focus by geometry, a moving focus halo,
// confirm / back / tab switching / primary action.

import { $$ } from "./dom";
import { reducedMotion } from "./motion";

export type Direction = "up" | "down" | "left" | "right";
export type Intent = Direction | "confirm" | "back" | "tabPrev" | "tabNext" | "primary";
export type InputMode = "mouse" | "keys" | "pad";

type IntentHandler = (intent: Intent) => boolean;

const root = document.documentElement;
const handlers: IntentHandler[] = [];
const modeSubs = new Set<(m: InputMode) => void>();
let mode: InputMode = "mouse";
let backAction: () => void = () => undefined;
let tabAction: (dir: 1 | -1) => void = () => undefined;

const FOCUSABLE =
  'a[href], button, summary, input:not([type="hidden"]), select, textarea, [tabindex]:not([tabindex="-1"]), [contenteditable="true"]';

export function configureIntents(opts: { back: () => void; tab: (dir: 1 | -1) => void }): void {
  backAction = opts.back;
  tabAction = opts.tab;
}

/** Screens push handlers to take intents first (return true when handled). Returns a remover. */
export function pushIntentHandler(fn: IntentHandler): () => void {
  handlers.push(fn);
  return () => {
    const i = handlers.lastIndexOf(fn);
    if (i >= 0) handlers.splice(i, 1);
  };
}

export function inputMode(): InputMode {
  return mode;
}

export function setInputMode(m: InputMode): void {
  if (m === mode) return;
  mode = m;
  root.dataset.input = m;
  for (const fn of modeSubs) fn(m);
  refreshHalo();
}

export function onInputMode(fn: (m: InputMode) => void): () => void {
  modeSubs.add(fn);
  return () => modeSubs.delete(fn);
}

export function dispatchIntent(intent: Intent, source: "keys" | "pad"): void {
  setInputMode(source);
  for (let i = handlers.length - 1; i >= 0; i--) {
    if (handlers[i](intent)) return;
  }
  switch (intent) {
    case "up":
    case "down":
    case "left":
    case "right":
      moveFocus(intent);
      break;
    case "confirm":
      activateFocused();
      break;
    case "back":
      backAction();
      break;
    case "tabPrev":
      tabAction(-1);
      break;
    case "tabNext":
      tabAction(1);
      break;
    case "primary":
      clickPrimary();
      break;
  }
}

// ---- Spatial navigation ------------------------------------------------------------------------

function isTyping(el: Element | null): boolean {
  if (!el || !(el instanceof HTMLElement)) return false;
  if (el.isContentEditable) return true;
  if (el instanceof HTMLTextAreaElement || el instanceof HTMLSelectElement) return true;
  if (el instanceof HTMLInputElement) {
    return !["button", "checkbox", "radio", "range", "submit", "reset", "color", "file"].includes(el.type);
  }
  return false;
}

export function isNavigable(el: HTMLElement): boolean {
  if ((el as HTMLButtonElement).disabled) return false;
  if (el.dataset.nav === "skip") return false;
  if (el.closest("[inert], [hidden], [aria-hidden='true']")) return false;
  const r = el.getBoundingClientRect();
  if (r.width < 2 || r.height < 2) return false;
  const cv = (el as unknown as { checkVisibility?: (o?: Record<string, boolean>) => boolean }).checkVisibility;
  if (typeof cv === "function" && !cv.call(el, { checkOpacity: true, checkVisibilityCSS: true })) return false;
  return true;
}

function candidates(): HTMLElement[] {
  const scopes = $$<HTMLElement>("[data-nav-scope]");
  const scope = scopes.length ? scopes[scopes.length - 1] : document.body;
  return $$<HTMLElement>(FOCUSABLE, scope).filter(isNavigable);
}

function gap(a0: number, a1: number, b0: number, b1: number): number {
  if (b1 < a0) return a0 - b1;
  if (b0 > a1) return b0 - a1;
  return 0;
}

function score(dir: Direction, a: DOMRect, b: DOMRect): number | null {
  const acx = a.left + a.width / 2;
  const acy = a.top + a.height / 2;
  const bcx = b.left + b.width / 2;
  const bcy = b.top + b.height / 2;
  let main: number;
  let cross: number;
  let align: number;
  if (dir === "down") {
    if (bcy <= acy + 1 || b.top < a.top + 2) return null;
    main = Math.max(0, b.top - a.bottom);
    cross = gap(a.left, a.right, b.left, b.right);
    align = Math.abs(bcx - acx);
  } else if (dir === "up") {
    if (bcy >= acy - 1 || b.bottom > a.bottom - 2) return null;
    main = Math.max(0, a.top - b.bottom);
    cross = gap(a.left, a.right, b.left, b.right);
    align = Math.abs(bcx - acx);
  } else if (dir === "right") {
    if (bcx <= acx + 1 || b.left < a.left + 2) return null;
    main = Math.max(0, b.left - a.right);
    cross = gap(a.top, a.bottom, b.top, b.bottom);
    align = Math.abs(bcy - acy);
  } else {
    if (bcx >= acx - 1 || b.right > a.right - 2) return null;
    main = Math.max(0, a.left - b.right);
    cross = gap(a.top, a.bottom, b.top, b.bottom);
    align = Math.abs(bcy - acy);
  }
  return main + cross * 2.5 + align * 0.15;
}

export function focusElement(el: HTMLElement): void {
  el.focus({ preventScroll: true });
  el.scrollIntoView({ block: "nearest", inline: "nearest", behavior: reducedMotion() ? "auto" : "smooth" });
  refreshHalo();
}

/** Focuses the screen's [data-autofocus], else its primary action, else the first control. */
export function focusFirst(scope: ParentNode = document): boolean {
  const pick =
    $$<HTMLElement>("[data-autofocus]", scope).find(isNavigable) ??
    $$<HTMLElement>("[data-primary]", scope).find(isNavigable) ??
    $$<HTMLElement>(FOCUSABLE, scope).find(isNavigable);
  if (pick) {
    focusElement(pick);
    return true;
  }
  return false;
}

/**
 * For pad and keyboard users: after a screen fills in asynchronously, put focus on its preferred
 * control if focus is nowhere useful (body, chrome, or an element that went away).
 */
export function settleFocus(scope: HTMLElement): void {
  if (mode === "mouse" || !scope.isConnected) return;
  const active = document.activeElement as HTMLElement | null;
  const lost = !active || active === document.body || !scope.contains(active) || !isNavigable(active);
  if (lost) focusFirst(scope);
}

function moveFocus(dir: Direction): void {
  const current = document.activeElement as HTMLElement | null;
  const list = candidates();
  if (!current || current === document.body || !list.includes(current)) {
    const screen = document.querySelector(".screen");
    if (!focusFirst(screen ?? document)) focusFirst();
    return;
  }
  const from = current.getBoundingClientRect();
  let best: HTMLElement | null = null;
  let bestScore = Infinity;
  for (const el of list) {
    if (el === current || el.contains(current) || current.contains(el)) continue;
    const s = score(dir, from, el.getBoundingClientRect());
    if (s !== null && s < bestScore) {
      bestScore = s;
      best = el;
    }
  }
  if (best) focusElement(best);
  else bump(dir);
}

function activateFocused(): void {
  const el = document.activeElement as HTMLElement | null;
  if (!el || el === document.body) {
    focusFirst(document.querySelector(".screen") ?? document);
    return;
  }
  if (isTyping(el)) {
    el.focus();
    return;
  }
  el.click();
}

function clickPrimary(): void {
  const btn = $$<HTMLElement>(".screen [data-primary]").find(isNavigable);
  if (!btn) return;
  focusElement(btn);
  btn.click();
}

// ---- Focus halo --------------------------------------------------------------------------------
// Liquid ember: the halo springs to the next control, stretching along the way it travels, and a
// short comet tail streaks from where it was. The halo takes the focused control's own shape (all
// four corners, so leaf-shaped controls get a leaf-shaped halo). Everything that moves is
// transform or opacity on two small fixed layers; the tail is one element, reused.

let halo: HTMLDivElement | null = null;
let haloRing: HTMLDivElement | null = null;
let tail: HTMLDivElement | null = null;
let tailStreak: HTMLDivElement | null = null;
let tailAnim: Animation | null = null;
let haloOn = false;
let haloFrame = 0;
let haloTarget: Element | null = null;
let lastCx = 0;
let lastCy = 0;
let observed: Element | null = null;
const resizeObserver = typeof ResizeObserver === "function" ? new ResizeObserver(() => refreshHalo()) : null;

function cornerRadii(target: HTMLElement, pad: number, w: number, h: number): string {
  const cs = getComputedStyle(target);
  const max = Math.min(w, h) / 2;
  const corner = (v: string) => {
    const n = parseFloat(v) || 0;
    return `${Math.round(Math.min(max, n > 0 ? n + pad : pad + 4))}px`;
  };
  return [cs.borderTopLeftRadius, cs.borderTopRightRadius, cs.borderBottomRightRadius, cs.borderBottomLeftRadius].map(corner).join(" ");
}

function comet(fromX: number, fromY: number, cx: number, cy: number, hw: number, hh: number): void {
  if (!tail || !tailStreak || reducedMotion() || typeof tailStreak.animate !== "function") return;
  const vx = cx - fromX;
  const vy = cy - fromY;
  // The head stops where the line meets the new control's edge, so the tail never crosses its label.
  const t = Math.min(vx ? hw / Math.abs(vx) : Infinity, vy ? hh / Math.abs(vy) : Infinity, 1);
  const toX = cx - vx * t;
  const toY = cy - vy * t;
  const dx = toX - fromX;
  const dy = toY - fromY;
  const dist = Math.hypot(dx, dy);
  if (dist < 24) return;
  // The tail's head sits on the new control and it points back to the old one.
  const len = Math.min(dist, 420);
  tail.style.width = `${Math.round(len)}px`;
  tail.style.transform = `translate(${Math.round(toX - len)}px, ${Math.round(toY - 3)}px) rotate(${Math.atan2(dy, dx).toFixed(4)}rad)`;
  tailAnim?.cancel();
  tailAnim = tailStreak.animate(
    [
      { transform: "scaleX(1)", opacity: 0.85 },
      { transform: "scaleX(0)", opacity: 0 },
    ],
    { duration: 340, easing: "cubic-bezier(.2,.7,.3,1)", fill: "both" },
  );
}

function stretch(dx: number, dy: number): void {
  if (!haloRing || reducedMotion() || typeof haloRing.animate !== "function") return;
  const dist = Math.hypot(dx, dy);
  if (dist < 24) return;
  const k = Math.min(0.14, dist / 2400);
  const horizontal = Math.abs(dx) >= Math.abs(dy);
  const s = horizontal ? `${(1 + k).toFixed(3)}, ${(1 - k * 0.6).toFixed(3)}` : `${(1 - k * 0.6).toFixed(3)}, ${(1 + k).toFixed(3)}`;
  haloRing.animate([{ transform: "scale(1, 1)" }, { transform: `scale(${s})`, offset: 0.32 }, { transform: "scale(1, 1)" }], {
    duration: 460,
    easing: "cubic-bezier(.3,.7,.3,1)",
  });
}

function placeHalo(): void {
  haloFrame = 0;
  if (!halo) return;
  const el = document.activeElement;
  const show = mode !== "mouse" && el instanceof HTMLElement && el !== document.body && el.dataset.halo !== "off" && isNavigable(el);
  if (observed !== el && resizeObserver) {
    if (observed) resizeObserver.unobserve(observed);
    observed = show ? el : null;
    if (observed) resizeObserver.observe(observed);
  }
  if (!show) {
    if (haloOn) halo.classList.remove("is-on");
    haloOn = false;
    haloTarget = null;
    return;
  }
  const target = el as HTMLElement;
  const r = target.getBoundingClientRect();
  const pad = 4;
  const w = Math.round(r.width + pad * 2);
  const hgt = Math.round(r.height + pad * 2);
  const cx = r.left + r.width / 2;
  const cy = r.top + r.height / 2;
  const moved = haloOn && haloTarget !== target;
  if (!haloOn) halo.classList.add("no-anim");
  halo.style.transform = `translate(${Math.round(r.left - pad)}px, ${Math.round(r.top - pad)}px)`;
  halo.style.width = `${w}px`;
  halo.style.height = `${hgt}px`;
  halo.style.borderRadius = cornerRadii(target, pad, w, hgt);
  halo.classList.add("is-on");
  if (!haloOn) {
    void halo.offsetWidth;
    halo.classList.remove("no-anim");
  }
  if (moved) {
    comet(lastCx, lastCy, cx, cy, w / 2, hgt / 2);
    stretch(cx - lastCx, cy - lastCy);
  }
  lastCx = cx;
  lastCy = cy;
  haloTarget = target;
  haloOn = true;
}

export function refreshHalo(): void {
  if (!haloFrame) haloFrame = requestAnimationFrame(placeHalo);
}

function bump(dir: Direction): void {
  if (!haloRing || !haloOn || reducedMotion()) return;
  // Nothing further that way: the liquid halo leans into the edge and springs back.
  const dx = dir === "left" ? -7 : dir === "right" ? 7 : 0;
  const dy = dir === "up" ? -7 : dir === "down" ? 7 : 0;
  const s = dx ? "0.94, 1.04" : "1.04, 0.94";
  haloRing.animate([{ transform: "none" }, { transform: `translate(${dx}px, ${dy}px) scale(${s})`, offset: 0.3 }, { transform: "none" }], { duration: 320, easing: "cubic-bezier(.3,.7,.3,1)" });
}

// ---- Wiring ------------------------------------------------------------------------------------

export function initInput(): void {
  root.dataset.input = mode;
  halo = document.createElement("div");
  halo.id = "focus-halo";
  halo.setAttribute("aria-hidden", "true");
  haloRing = document.createElement("div");
  haloRing.className = "halo-ring";
  halo.appendChild(haloRing);
  tail = document.createElement("div");
  tail.id = "focus-tail";
  tail.setAttribute("aria-hidden", "true");
  tailStreak = document.createElement("div");
  tailStreak.className = "tail-streak";
  tail.appendChild(tailStreak);
  document.body.append(tail, halo);

  document.addEventListener("focusin", refreshHalo);
  document.addEventListener("focusout", refreshHalo);
  window.addEventListener("scroll", refreshHalo, { capture: true, passive: true });
  window.addEventListener("resize", refreshHalo);
  document.addEventListener("transitionend", refreshHalo);

  document.addEventListener("pointerdown", (e) => {
    if (e.pointerType === "mouse" || e.pointerType === "pen" || e.pointerType === "touch") setInputMode("mouse");
  });
  let lastMove = 0;
  document.addEventListener("pointermove", (e) => {
    if (mode === "mouse" || e.pointerType !== "mouse") return;
    const now = performance.now();
    if (Math.abs(e.movementX) + Math.abs(e.movementY) > 6 && now - lastMove < 120) setInputMode("mouse");
    lastMove = now;
  });

  document.addEventListener("keydown", (e) => {
    if (e.defaultPrevented || e.altKey || e.metaKey) return;
    const target = e.target as Element | null;
    const typing = isTyping(target);
    if (e.key === "Tab" && e.ctrlKey) {
      e.preventDefault();
      dispatchIntent(e.shiftKey ? "tabPrev" : "tabNext", "keys");
      return;
    }
    if (e.key === "Tab") {
      setInputMode("keys");
      return;
    }
    if (e.key === "Escape") {
      e.preventDefault();
      // In a text field, Escape only leaves the field, so a half-written note is never lost.
      if (typing && target instanceof HTMLElement) {
        setInputMode("keys");
        target.blur();
        return;
      }
      dispatchIntent("back", "keys");
      return;
    }
    if (typing || e.ctrlKey) return;
    const dir: Direction | null =
      e.key === "ArrowUp" ? "up" : e.key === "ArrowDown" ? "down" : e.key === "ArrowLeft" ? "left" : e.key === "ArrowRight" ? "right" : null;
    if (dir) {
      e.preventDefault();
      dispatchIntent(dir, "keys");
      return;
    }
    if (e.key === "Enter" || e.key === " ") setInputMode("keys");
  });
}
