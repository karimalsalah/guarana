// Shared app state: cached backend reads and a few small observable values.

import { api, events } from "./api";
import type { Check, Controller, Found, Game, LauncherState, Overrides, Profile, SystemInfo, WarmProgress } from "./types";

export class Signal<T> {
  private subs = new Set<(v: T) => void>();
  value: T;

  constructor(value: T) {
    this.value = value;
  }

  set(v: T): void {
    this.value = v;
    for (const fn of this.subs) fn(v);
  }

  /** Subscribes; unsubscribes automatically when `signal` aborts (a screen leaving). */
  subscribe(fn: (v: T) => void, signal?: AbortSignal): () => void {
    this.subs.add(fn);
    const off = () => {
      this.subs.delete(fn);
    };
    signal?.addEventListener("abort", off, { once: true });
    return off;
  }
}

interface Cached<T> {
  get(force?: boolean): Promise<T>;
  peek(): T | undefined;
  set(v: T): void;
}

function cached<T>(fetch: () => Promise<T>): Cached<T> {
  let pending: Promise<T> | null = null;
  let value: T | undefined;
  return {
    get(force = false) {
      if (!pending || force) {
        const next = fetch();
        pending = next;
        next.then(
          (v) => {
            if (pending === next) value = v;
          },
          () => {
            if (pending === next) pending = null;
          },
        );
      }
      return pending;
    },
    peek: () => value,
    set(v: T) {
      value = v;
      pending = Promise.resolve(v);
    },
  };
}

export const systemInfo = cached<SystemInfo>(() => api.detectSystem());
export const profiles = cached<Profile[]>(() => api.listProfiles());
export const launcher = cached<LauncherState>(() => api.getState());
export const library = cached<Game[]>(() => api.listGames());

/** Last results of a file scan and of the doctor, shared between Scan, Home and Doctor. */
export const found = new Signal<Found | null>(null);
export const checks = new Signal<Check[] | null>(null);

/** The profile the user picked on Profile, and their per-toggle overrides (not applied yet). */
export const profileDraft = {
  id: null as string | null,
  overrides: {} as Overrides,
};

export function recommendedProfile(list: Profile[], sys?: SystemInfo): Profile | undefined {
  const hint = sys?.device.profile_hint ?? null;
  return (
    list.find((p) => p.recommended) ??
    (hint ? list.find((p) => p.id === hint) : undefined) ??
    list.find((p) => p.id === "safe-defaults") ??
    list[0]
  );
}

export function activeProfileId(list: Profile[], state?: LauncherState, sys?: SystemInfo): string | undefined {
  return profileDraft.id ?? state?.profile_id ?? recommendedProfile(list, sys)?.id;
}

// ---- Controllers -----------------------------------------------------------------------------------

export const controllers = new Signal<Controller[]>([]);
let controllersStarted = false;

export function initControllers(): void {
  if (controllersStarted) return;
  controllersStarted = true;
  const refresh = () =>
    api.listControllers().then(
      (list) => controllers.set(Array.isArray(list) ? list : []),
      () => controllers.set([]),
    );
  void refresh();
  void events
    .controllersChanged((payload) => {
      if (Array.isArray(payload)) controllers.set(payload as Controller[]);
      else void refresh();
    })
    .catch(() => undefined);
}

// ---- Shader warm-ups (warm_shaders, and the automatic run after a driver change) ----------------------

/** Latest warm://progress per title id. An entry leaves a moment after its "done" event. */
export const warmStatus = new Signal<Record<string, WarmProgress>>({});
let warmStarted = false;

export function initWarm(): void {
  if (warmStarted) return;
  warmStarted = true;
  void events
    .warmProgress((p) => {
      warmStatus.set({ ...warmStatus.value, [p.title_id]: p });
      if (p.stage === "done") {
        setTimeout(() => {
          const next = { ...warmStatus.value };
          if (next[p.title_id]?.stage === "done") delete next[p.title_id];
          warmStatus.set(next);
        }, 1500);
      }
    })
    .catch(() => undefined);
}
