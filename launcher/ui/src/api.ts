// The only module that knows command and event names (launcher/CONTRACT.md).
// Inside Tauri it uses @tauri-apps/api v2; in a plain browser it loads the mock backend.

import type {
  AppliedProfile, Channel, Check, Controller, Found, Game, GameExited, ImportResult, InstallResult,
  LaunchArgs, LauncherState, Overrides, Profile, Progress, Report, ReportKind, SystemInfo, UpdateInfo,
  WarmProgress, WarmResult, AiService, AskAiResult, ModeId, ModeStatus, StopResult, Credits, ShareStatus, ShareLink,
} from "./types";

export type Unlisten = () => void;

export interface Backend {
  invoke<T>(cmd: string, args?: Record<string, unknown>): Promise<T>;
  listen<T>(event: string, handler: (payload: T) => void): Promise<Unlisten>;
  appVersion(): Promise<string>;
}

export const inTauri: boolean = typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;

/**
 * Argument keys go out in snake_case (`profile_id`, `fix_id`, `title_id`), as CONTRACT.md writes them:
 * the Rust commands are declared with `rename_all = "snake_case"`. Set this to true only if they ever
 * go back to Tauri's camelCase default.
 */
const CAMEL_CASE_ARGS = false as boolean;

let backendPromise: Promise<Backend> | null = null;

export function backend(): Promise<Backend> {
  if (!backendPromise) backendPromise = inTauri ? tauriBackend() : mockBackend();
  return backendPromise;
}

async function tauriBackend(): Promise<Backend> {
  const [core, event, app] = await Promise.all([
    import("@tauri-apps/api/core"),
    import("@tauri-apps/api/event"),
    import("@tauri-apps/api/app"),
  ]);
  return {
    invoke: <T>(cmd: string, args?: Record<string, unknown>) => core.invoke<T>(cmd, args),
    listen: <T>(name: string, handler: (payload: T) => void) => event.listen<T>(name, (e) => handler(e.payload)),
    appVersion: () => app.getVersion(),
  };
}

async function mockBackend(): Promise<Backend> {
  const mock = await import("./mock");
  return mock.createMockBackend();
}

function toArgs(o: Record<string, unknown>): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(o)) {
    if (v === undefined) continue;
    const key = CAMEL_CASE_ARGS ? k.replace(/_([a-z])/g, (_m: string, c: string) => c.toUpperCase()) : k;
    out[key] = v;
  }
  return out;
}

async function call<T>(cmd: string, args?: Record<string, unknown>): Promise<T> {
  const b = await backend();
  return b.invoke<T>(cmd, args ? toArgs(args) : undefined);
}

/** Subscribes to a backend event. Pass the screen's AbortSignal and it unsubscribes on leave. */
async function on<T>(name: string, handler: (payload: T) => void, signal?: AbortSignal): Promise<Unlisten> {
  const b = await backend();
  const unlisten = await b.listen<T>(name, handler);
  if (signal) {
    if (signal.aborted) unlisten();
    else signal.addEventListener("abort", () => unlisten(), { once: true });
  }
  return unlisten;
}

export const api = {
  detectSystem: () => call<SystemInfo>("detect_system"),
  listProfiles: () => call<Profile[]>("list_profiles"),
  getState: () => call<LauncherState>("get_state"),
  /** Without a channel the backend checks the installed version's channel. */
  checkUpdates: (channel?: Channel) => call<UpdateInfo>("check_updates", channel ? { channel } : undefined),
  installVersion: (version: string, channel: Channel) => call<InstallResult>("install_version", { version, channel }),
  setCurrentVersion: (version: string) => call<LauncherState>("set_current_version", { version }),
  locateFiles: (scope: "quick" | "deep", roots?: string[]) =>
    call<Found>("locate_files", roots && roots.length ? { scope, roots } : { scope }),
  importKeys: (path: string) => call<ImportResult>("import_keys", { path }),
  addGameFolder: (path: string) => call<Game[]>("add_game_folder", { path }),
  listGames: () => call<Game[]>("list_games"),
  applyProfile: (profileId: string, overrides?: Overrides) =>
    call<AppliedProfile>("apply_profile", { profile_id: profileId, overrides }),
  runDoctor: () => call<Check[]>("run_doctor"),
  applyFix: (fixId: string) => call<Check>("apply_fix", { fix_id: fixId }),
  prefetchCaches: (titleId: string) => call<{ bytes_warmed: number }>("prefetch_caches", { title_id: titleId }),
  launchGame: (a: LaunchArgs) => call<{ pid: number }>("launch_game", { ...a }),
  collectReport: (kind: ReportKind, note?: string) => call<Report>("collect_report", { kind, note: note || undefined }),
  openPath: (path: string) => call<void>("open_path", { path }),
  openUrl: (url: string) => {
    if (!/^https:\/\//i.test(url)) return Promise.reject(new Error("Only https links can be opened."));
    return call<void>("open_url", { url });
  },
  listControllers: () => call<Controller[]>("list_controllers"),
  /** Builds a title's pipelines ahead of time ("Prepare shaders"); streams warm://progress. */
  warmShaders: (titleId: string) => call<WarmResult>("warm_shaders", { title_id: titleId }),
  /** Ends the running shader warm-up and reports. */
  stopWarmShaders: () => call<StopResult>("stop_warm_shaders"),
  /** The Windows folder picker; null when cancelled. */
  pickFolder: (title?: string) => call<string | null>("pick_folder", title ? { title } : undefined),
  /** The mode, what it chose and why, community data and the autopilot history. */
  getMode: () => call<ModeStatus>("get_mode"),
  setMode: (mode: ModeId) => call<ModeStatus>("set_mode", { mode }),
  autopilotUndo: () => call<ModeStatus>("autopilot_undo"),
  autopilotEnable: (enabled: boolean) => call<ModeStatus>("autopilot_enable", { enabled }),
  /** Builds a scrubbed summary and opens a prefilled chat in the browser. */
  askAi: (service: AiService, issues?: string[], question?: string) =>
    call<AskAiResult>("ask_ai", { service, issues: issues && issues.length ? issues : undefined, question: question || undefined }),
  /** The hall of fame for the About screen (cached a day, embedded copy offline). */
  getCredits: () => call<Credits>("get_credits"),
  /** "Share my numbers": the card's data, or none. */
  getShareStatus: () => call<ShareStatus>("get_share_status"),
  /** The prefilled device-report link for the offer; the UI opens it. Nothing is sent. */
  shareOpen: () => call<ShareLink>("share_open"),
  /** "Not now": drops the offer. */
  shareDismiss: () => call<ShareStatus>("share_dismiss"),
  /** The setting to stop (or resume) asking after a game. */
  shareSetPrompt: (enabled: boolean) => call<ShareStatus>("share_set_prompt", { enabled }),
  appVersion: async () => (await backend()).appVersion(),
};

export const events = {
  locateProgress: (h: (p: Progress) => void, signal?: AbortSignal) => on<Progress>("locate://progress", h, signal),
  doctorProgress: (h: (p: Progress) => void, signal?: AbortSignal) => on<Progress>("doctor://progress", h, signal),
  installProgress: (h: (p: Progress) => void, signal?: AbortSignal) => on<Progress>("install://progress", h, signal),
  gameExited: (h: (p: GameExited) => void, signal?: AbortSignal) => on<GameExited>("game://exited", h, signal),
  /** Carries the new Controller[]; anything else makes the UI call list_controllers again. */
  controllersChanged: (h: (p: unknown) => void, signal?: AbortSignal) => on<unknown>("controller://changed", h, signal),
  warmProgress: (h: (p: WarmProgress) => void, signal?: AbortSignal) => on<WarmProgress>("warm://progress", h, signal),
  shareChanged: (h: (p: ShareStatus) => void, signal?: AbortSignal) => on<ShareStatus>("share://changed", h, signal),
  autopilotChanged: (h: (p: ModeStatus) => void, signal?: AbortSignal) => on<ModeStatus>("autopilot://changed", h, signal),
};

/** Turns whatever a rejected invoke carries (string, Error, {message}) into one readable line. */
export function errorText(e: unknown): string {
  if (typeof e === "string") return e;
  if (e instanceof Error) return e.message;
  if (e && typeof e === "object") {
    const o = e as Record<string, unknown>;
    if (typeof o.message === "string") return o.message;
    if (typeof o.error === "string") return o.error;
    try {
      return JSON.stringify(e);
    } catch {
      /* fall through */
    }
  }
  return "Something went wrong.";
}

/** True when the backend does not implement a command yet (Tauri: "command x not found"). */
export function isMissingCommand(e: unknown): boolean {
  return /not found|unknown command|not allowed/i.test(errorText(e));
}
