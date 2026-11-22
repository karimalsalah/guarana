// Screens are lazy modules. Navigation swaps the outlet inside a view transition, keeps a back
// stack, moves focus and announces the new screen.

import { focusFirst, inputMode, pushIntentHandler, refreshHalo, type Intent } from "./input";
import { viewTransition, type TransitionKind } from "./motion";

export type ScreenId = "welcome" | "scan" | "profile" | "home" | "launch" | "updates" | "doctor" | "report" | "about";
export type TabId = "home" | "profile" | "updates" | "doctor";
export type Chrome = "full" | "setup" | "none";

export const TABS: { id: TabId; label: string }[] = [
  { id: "home", label: "Home" },
  { id: "profile", label: "Profile" },
  { id: "updates", label: "Updates" },
  { id: "doctor", label: "Doctor" },
];

export interface ScreenContext {
  params: Record<string, unknown>;
  /** Aborted when the screen is left: pass it to listeners and subscriptions. */
  signal: AbortSignal;
  /** Runs on leave, before the next screen renders. */
  onLeave(fn: () => void): void;
  /** Takes intents (back, tabs, ...) before the global handlers while this screen is shown. */
  onIntent(fn: (intent: Intent) => boolean): void;
  /** Runs once the screen's entrance transition has finished. */
  onEntered(fn: () => void): void;
}

export interface ScreenDef {
  title: string;
  /** Static, or decided per visit (Scan and Profile are setup steps on first run, normal screens later). */
  chrome: Chrome | ((params: Record<string, unknown>) => Chrome);
  tab?: TabId;
  /** Builds the screen synchronously (skeletons are fine) and fills it in asynchronously. */
  render(ctx: ScreenContext): HTMLElement;
  /** Called after the transition finished, e.g. to start entrance animations. */
  entered?(): void;
  /** Called right before leaving, while the old screen is still on the page (e.g. to tag elements for a morph). */
  leaving?(to: ScreenId, params: Record<string, unknown>): void;
}

const loaders: Record<ScreenId, () => Promise<{ default: ScreenDef }>> = {
  welcome: () => import("./screens/welcome"),
  scan: () => import("./screens/scan"),
  profile: () => import("./screens/profile"),
  home: () => import("./screens/home"),
  launch: () => import("./screens/launch"),
  updates: () => import("./screens/updates"),
  doctor: () => import("./screens/doctor"),
  report: () => import("./screens/report"),
  about: () => import("./screens/about"),
};

const modules = new Map<ScreenId, Promise<ScreenDef>>();

export function loadScreen(id: ScreenId): Promise<ScreenDef> {
  let p = modules.get(id);
  if (!p) {
    p = loaders[id]().then((m) => m.default);
    p.catch(() => modules.delete(id));
    modules.set(id, p);
  }
  return p;
}

/** Starts downloading a screen's code without showing it (e.g. Home while Scan runs). */
export function preload(id: ScreenId): void {
  void loadScreen(id).catch(() => undefined);
}

interface Current {
  id: ScreenId;
  def: ScreenDef;
  chrome: Chrome;
  el: HTMLElement;
  params: Record<string, unknown>;
  controller: AbortController;
  cleanups: (() => void)[];
  entered: (() => void)[];
}

interface Entry {
  id: ScreenId;
  params: Record<string, unknown>;
}

export interface GoOptions {
  params?: Record<string, unknown>;
  transition?: TransitionKind;
  /** Don't push the current screen on the back stack. */
  replace?: boolean;
  /** Clear the back stack (used by top-level tabs). */
  reset?: boolean;
}

type ShellHooks = {
  outlet: HTMLElement;
  setChrome(chrome: Chrome, tab: TabId | undefined): void;
  announce(text: string): void;
  loading(on: boolean): void;
};

let shell: ShellHooks | null = null;
let current: Current | null = null;
const stack: Entry[] = [];
let navSeq = 0;
const navSubs = new Set<(id: ScreenId) => void>();

export function attachShell(hooks: ShellHooks): void {
  shell = hooks;
}

export function currentScreen(): ScreenId | null {
  return current?.id ?? null;
}

export function onNavigate(fn: (id: ScreenId) => void): () => void {
  navSubs.add(fn);
  return () => navSubs.delete(fn);
}

export async function go(id: ScreenId, opts: GoOptions = {}): Promise<void> {
  if (!shell) throw new Error("router: shell not attached");
  const hooks = shell;
  const seq = ++navSeq;
  const params = opts.params ?? {};

  const slow = setTimeout(() => hooks.loading(true), 120);
  const def = await loadScreen(id).finally(() => {
    clearTimeout(slow);
    hooks.loading(false);
  });
  if (seq !== navSeq) return;

  const from = current;
  if (from) {
    if (opts.reset) stack.length = 0;
    else if (!opts.replace) stack.push({ id: from.id, params: from.params });
    from.def.leaving?.(id, params);
  }
  if (opts.reset && id !== "home" && def.tab) stack.push({ id: "home", params: {} });

  const transition = opts.transition ?? (from ? "forward" : "none");
  const controller = new AbortController();
  const chrome: Chrome = typeof def.chrome === "function" ? def.chrome(params) : def.chrome;
  const next: Current = { id, def, chrome, el: document.createElement("section"), params, controller, cleanups: [], entered: [] };
  const ctx: ScreenContext = {
    params,
    signal: controller.signal,
    onLeave: (fn) => next.cleanups.push(fn),
    onIntent: (fn) => next.cleanups.push(pushIntentHandler(fn)),
    onEntered: (fn) => next.entered.push(fn),
  };

  await viewTransition(() => {
    if (from) {
      from.controller.abort();
      for (const fn of from.cleanups.splice(0)) fn();
    }
    const el = def.render(ctx);
    el.classList.add("screen");
    el.dataset.screen = id;
    next.el = el;
    hooks.outlet.replaceChildren(el);
    hooks.setChrome(chrome, chrome === "full" ? def.tab : undefined);
    current = next;
    document.title = id === "home" ? "Guarana" : `${def.title} · Guarana`;
    const main = document.getElementById("main");
    if (main) main.scrollTop = 0;
    if (inputMode() !== "mouse") focusFirst(el);
    else focusHeading(el);
  }, transition);

  if (current !== next) return;
  hooks.announce(def.title);
  refreshHalo();
  def.entered?.();
  for (const fn of next.entered.splice(0)) fn();
  for (const fn of navSubs) fn(id);
}

function focusHeading(el: HTMLElement): void {
  const heading = el.querySelector<HTMLElement>("h1");
  if (!heading) return;
  heading.tabIndex = -1;
  heading.focus({ preventScroll: true });
}

export function canGoBack(): boolean {
  return stack.length > 0;
}

export function back(): void {
  const prev = stack.pop();
  if (!prev) return;
  void go(prev.id, { params: prev.params, transition: "back", replace: true });
}

export function goTab(id: TabId): void {
  if (!current) return;
  if (current.id === id) return;
  const order = TABS.map((t) => t.id);
  const fromTab = current.def.tab;
  const a = fromTab ? order.indexOf(fromTab) : -1;
  const b = order.indexOf(id);
  const transition: TransitionKind = a < 0 ? "fade" : b > a ? "tab-right" : "tab-left";
  void go(id, { transition, reset: true });
}

export function cycleTab(dir: 1 | -1): void {
  if (!current || current.chrome !== "full") return;
  const order = TABS.map((t) => t.id);
  const i = current.def.tab ? order.indexOf(current.def.tab) : 0;
  const next = order[(i + dir + order.length) % order.length];
  goTab(next);
}
