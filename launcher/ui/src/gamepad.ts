// Gamepad API: polling (only while the window is focused and no game runs), per-pad layouts and
// button glyphs. Standard mapping is positional: 0 bottom, 1 right, 2 left, 3 top face button.

import { h, svg } from "./dom";
import { dispatchIntent, type Direction } from "./input";
import { isIdle, isPaused, onIdleChange, onPauseChange } from "./motion";
import { Signal } from "./state";
import type { ControllerKind } from "./types";

export interface PadLayout {
  /** Standard-mapping index of the button that selects. */
  confirm: number;
  /** Standard-mapping index of the button that goes back. */
  back: number;
  /** Labels of the face buttons by standard index: bottom, right, left, top. */
  face: [string, string, string, string];
  shoulders: [string, string];
  menu: string;
}

// Nintendo-style pads confirm with A, which sits on the right; Xbox and PlayStation pads confirm with
// the bottom button. Index and label for each kind live in this one table.
export const LAYOUTS: Record<ControllerKind, PadLayout> = {
  xbox: { confirm: 0, back: 1, face: ["A", "B", "X", "Y"], shoulders: ["LB", "RB"], menu: "Menu" },
  playstation: { confirm: 0, back: 1, face: ["cross", "circle", "square", "triangle"], shoulders: ["L1", "R1"], menu: "Options" },
  switch: { confirm: 1, back: 0, face: ["B", "A", "Y", "X"], shoulders: ["L", "R"], menu: "+" },
  generic: { confirm: 0, back: 1, face: ["dot", "dot", "dot", "dot"], shoulders: ["L", "R"], menu: "Start" },
};

export function kindFromId(id: string): ControllerKind {
  const s = id.toLowerCase();
  if (/xinput|xbox|vendor: 045e/.test(s)) return "xbox";
  if (/vendor: 054c|dualsense|dualshock|playstation|ps[345]/.test(s)) return "playstation";
  if (/vendor: 057e|pro controller|joy-?con|nintendo|switch/.test(s)) return "switch";
  if (/wireless controller/.test(s)) return "playstation";
  return "generic";
}

/** "DualSense Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 0ce6)" -> "DualSense Wireless Controller". */
export function padName(id: string): string {
  const name = id.replace(/\s*\((?:STANDARD GAMEPAD|XInput)[^)]*\)\s*/gi, "").replace(/\s*\(Vendor:[^)]*\)/i, "").trim();
  return name || "Controller";
}

/** Kind of the pad used last (glyphs follow it). null until a pad is seen. */
export const activeKind = new Signal<ControllerKind | null>(null);
/** Pads the Gamepad API sees right now (fallback when list_controllers has nothing). */
export const browserPads = new Signal<{ index: number; id: string; kind: ControllerKind }[]>([]);

export interface PadButtonEvent {
  index: number;
  pressed: boolean;
  kind: ControllerKind;
}
const buttonSubs = new Set<(e: PadButtonEvent) => void>();

export function onPadButton(fn: (e: PadButtonEvent) => void, signal?: AbortSignal): () => void {
  buttonSubs.add(fn);
  const off = () => {
    buttonSubs.delete(fn);
  };
  signal?.addEventListener("abort", off, { once: true });
  return off;
}

const REPEAT_DELAY = 380;
const REPEAT_RATE = 110;

interface PadState {
  buttons: boolean[];
  dir: Direction | null;
  since: number;
  last: number;
}

const states = new Map<number, PadState>();
let raf = 0;
let lastActive = -1;

function setActiveKind(kind: ControllerKind | null): void {
  if (activeKind.value === kind) return;
  activeKind.set(kind);
}

function connected(): Gamepad[] {
  const list = typeof navigator.getGamepads === "function" ? navigator.getGamepads() : [];
  return Array.from(list).filter((p): p is Gamepad => !!p && p.connected);
}

function syncPads(): void {
  const pads = connected();
  browserPads.set(pads.map((p) => ({ index: p.index, id: p.id, kind: kindFromId(p.id) })));
  if (!pads.length) {
    setActiveKind(null);
    return;
  }
  if (!activeKind.value || !pads.some((p) => p.index === lastActive)) {
    lastActive = pads[0].index;
    setActiveKind(kindFromId(pads[0].id));
  }
}

let suspended = false;

/** Stops reading pads (from launch_game until game://exited), so in-game presses never reach the launcher. */
export function setPadSuspended(on: boolean): void {
  if (suspended === on) return;
  suspended = on;
  if (on) stop();
  else start();
}

function canPoll(): boolean {
  return !suspended && !isPaused() && !isIdle() && document.hasFocus();
}

/** Takes the current button state as the baseline, so a held button never fires on resume. */
function baseline(): void {
  for (const p of connected()) {
    states.set(p.index, { buttons: p.buttons.map((b) => b.pressed), dir: null, since: 0, last: 0 });
  }
}

function start(): void {
  if (raf || !canPoll() || !connected().length) return;
  baseline();
  raf = requestAnimationFrame(tick);
}

function stop(): void {
  if (raf) cancelAnimationFrame(raf);
  raf = 0;
}

function tick(t: number): void {
  raf = 0;
  if (!canPoll()) return;
  const pads = connected();
  if (!pads.length) return;
  for (const p of pads) poll(p, t);
  raf = requestAnimationFrame(tick);
}

function poll(p: Gamepad, t: number): void {
  const kind = kindFromId(p.id);
  const layout = LAYOUTS[kind];
  const s: PadState = states.get(p.index) ?? { buttons: [], dir: null, since: 0, last: 0 };
  const now = p.buttons.map((b) => b.pressed || b.value > 0.6);

  for (let i = 0; i < now.length; i++) {
    const was = s.buttons[i] ?? false;
    if (now[i] === was) continue;
    if (now[i]) {
      lastActive = p.index;
      setActiveKind(kind);
    }
    for (const fn of buttonSubs) fn({ index: i, pressed: now[i], kind });
    if (!now[i]) continue;
    if (i === layout.confirm) dispatchIntent("confirm", "pad");
    else if (i === layout.back) dispatchIntent("back", "pad");
    else if (i === 4) dispatchIntent("tabPrev", "pad");
    else if (i === 5) dispatchIntent("tabNext", "pad");
    else if (i === 9) dispatchIntent("primary", "pad");
  }
  s.buttons = now;

  let dir: Direction | null = null;
  if (now[12]) dir = "up";
  else if (now[13]) dir = "down";
  else if (now[14]) dir = "left";
  else if (now[15]) dir = "right";
  else {
    const x = p.axes[0] ?? 0;
    const y = p.axes[1] ?? 0;
    const threshold = s.dir ? 0.4 : 0.6;
    if (Math.max(Math.abs(x), Math.abs(y)) > threshold) {
      dir = Math.abs(x) > Math.abs(y) ? (x > 0 ? "right" : "left") : y > 0 ? "down" : "up";
    }
  }
  if (dir !== s.dir) {
    s.dir = dir;
    s.since = t;
    if (dir) {
      lastActive = p.index;
      setActiveKind(kind);
      dispatchIntent(dir, "pad");
      s.last = t;
    }
  } else if (dir && t - s.since > REPEAT_DELAY && t - s.last > REPEAT_RATE) {
    dispatchIntent(dir, "pad");
    s.last = t;
  }

  const ry = p.axes[3] ?? 0;
  if (Math.abs(ry) > 0.25) {
    const main = document.getElementById("main");
    main?.scrollBy({ top: ry * 18 });
  }
  states.set(p.index, s);
}

export function initGamepad(): void {
  if (typeof navigator.getGamepads !== "function") return;
  window.addEventListener("gamepadconnected", () => {
    syncPads();
    start();
  });
  window.addEventListener("gamepaddisconnected", () => {
    syncPads();
    if (!connected().length) stop();
  });
  window.addEventListener("focus", start);
  window.addEventListener("blur", stop);
  onIdleChange((idle) => (idle ? stop() : start()));
  onPauseChange((paused) => (paused ? stop() : start()));
  syncPads();
  start();
}

/** A short, light rumble on the pad used last (no-op where unsupported). */
export function rumble(ms = 70): void {
  const pad = connected().find((p) => p.index === lastActive);
  const act = (pad as unknown as { vibrationActuator?: { playEffect?: (type: string, params: Record<string, number>) => Promise<unknown> } } | undefined)
    ?.vibrationActuator;
  act?.playEffect?.("dual-rumble", { duration: ms, startDelay: 0, strongMagnitude: 0.15, weakMagnitude: 0.45 })?.catch(() => undefined);
}

// ---- Glyphs ----------------------------------------------------------------------------------------
// Each glyph holds one variant per pad kind; CSS shows the one matching html[data-pad].

export type GlyphRole = "confirm" | "back" | "tabPrev" | "tabNext" | "menu";

const PS_SHAPES: Record<string, string> = {
  cross: '<path d="M8.2 8.2l7.6 7.6M15.8 8.2l-7.6 7.6"/>',
  circle: '<circle cx="12" cy="12" r="4.6"/>',
  square: '<rect x="7.6" y="7.6" width="8.8" height="8.8" rx=".6"/>',
  triangle: '<path d="M12 7.2l5 8.6H7z"/>',
};

function faceVariant(kind: ControllerKind, label: string, position: number): HTMLElement {
  const el = h("span", { class: `g g-${kind} g-face` });
  if (kind === "playstation") {
    el.appendChild(svg(`<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${PS_SHAPES[label] ?? ""}</svg>`));
  } else if (kind === "generic") {
    const dots = [0, 1, 2, 3]
      .map((i) => {
        const pos = [[12, 17.5], [17.5, 12], [6.5, 12], [12, 6.5]][i];
        return `<circle cx="${pos[0]}" cy="${pos[1]}" r="2.3" ${i === position ? 'fill="currentColor"' : 'fill="none" stroke="currentColor" stroke-width="1.4"'}/>`;
      })
      .join("");
    el.appendChild(svg(`<svg viewBox="0 0 24 24" aria-hidden="true">${dots}</svg>`));
  } else {
    el.textContent = label;
  }
  return el;
}

/** A button hint glyph that follows the active pad kind. Decorative: pair it with visible text. */
export function glyph(role: GlyphRole): HTMLElement {
  const wrap = h("span", { class: `glyph glyph-${role}`, "aria-hidden": "true" });
  const kinds: ControllerKind[] = ["xbox", "playstation", "switch", "generic"];
  for (const kind of kinds) {
    const layout = LAYOUTS[kind];
    if (role === "confirm" || role === "back") {
      const index = role === "confirm" ? layout.confirm : layout.back;
      wrap.appendChild(faceVariant(kind, layout.face[index], index));
    } else {
      const text = role === "menu" ? layout.menu : layout.shoulders[role === "tabPrev" ? 0 : 1];
      wrap.appendChild(h("span", { class: `g g-${kind} g-pill`, text }));
    }
  }
  return wrap;
}

/** One face button (standard index 0-3) drawn for one pad kind, for the controller card. */
export function faceButton(index: number, kind: ControllerKind): HTMLElement {
  const wrap = h("span", { class: "face-btn", "data-index": index, "aria-hidden": "true" });
  const variant = faceVariant(kind, LAYOUTS[kind].face[index], index);
  variant.classList.remove("g");
  wrap.appendChild(variant);
  return wrap;
}

/** The pad kind glyphs should follow: the pad used last, else the first one the backend lists. */
export function syncPadAttribute(backendKinds: ControllerKind[]): void {
  const kind = activeKind.value ?? backendKinds[0] ?? null;
  if (kind) document.documentElement.dataset.pad = kind;
  else delete document.documentElement.dataset.pad;
}
