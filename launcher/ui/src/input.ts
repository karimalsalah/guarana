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
  'a[href], button, input:not([type="hidden"]), select, textarea, [tabindex]:not([tabindex="-1"]), [contenteditable="true"]';

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

let halo: HTMLDivElement | null = null;
let haloOn = false;
let haloFrame = 0;
let observed: Element | null = null;
const resizeObserver = typeof ResizeObserver === "function" ? new ResizeObserver(() => refreshHalo()) : null;

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
    return;
  }
  const target = el as HTMLElement;
  const r = target.getBoundingClientRect();
  const pad = 4;
  const radius = parseFloat(getComputedStyle(target).borderTopLeftRadius) || 8;
  if (!haloOn) halo.classList.add("no-anim");
  halo.style.transform = `translate(${Math.round(r.left - pad)}px, ${Math.round(r.top - pad)}px)`;
  halo.style.width = `${Math.round(r.width + pad * 2)}px`;
  halo.style.height = `${Math.round(r.height + pad * 2)}px`;
  halo.style.borderRadius = `${Math.min(radius + pad, (r.height + pad * 2) / 2)}px`;
  halo.classList.add("is-on");
  if (!haloOn) {
    void halo.offsetWidth;
    halo.classList.remove("no-anim");
  }
  haloOn = true;
}

export function refreshHalo(): void {
  if (!haloFrame) haloFrame = requestAnimationFrame(placeHalo);
}

function bump(dir: Direction): void {
  if (!halo || !haloOn || reducedMotion()) return;
  const dx = dir === "left" ? -6 : dir === "right" ? 6 : 0;
  const dy = dir === "up" ? -6 : dir === "down" ? 6 : 0;
  halo.animate([{ translate: "0 0" }, { translate: `${dx}px ${dy}px` }, { translate: "0 0" }], { duration: 220, easing: "ease-out" });
}

// ---- Wiring ------------------------------------------------------------------------------------

export function initInput(): void {
  root.dataset.input = mode;
  halo = document.createElement("div");
  halo.id = "focus-halo";
  halo.setAttribute("aria-hidden", "true");
  document.body.appendChild(halo);

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
