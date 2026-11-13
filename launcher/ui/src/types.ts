// Types from launcher/CONTRACT.md. Field names match the Rust side (snake_case).
// Additions that are not in the contract yet are marked "Proposed".

export type Vendor = "intel" | "amd" | "nvidia" | "other";

export interface Gpu {
  name: string;
  vendor: Vendor;
  driver_version: string;
  is_integrated: boolean;
  dedicated_mb: number;
  shared_mb: number;
  vulkan: boolean;
  arch_hint?: string;
}

export interface SystemInfo {
  os: { name: string; build: number; display_version: string };
  cpu: { name: string; cores: number; threads: number; avx2: boolean; avx512: boolean; hybrid: boolean };
  ram_gb: number;
  pagefile_gb: number;
  gpus: Gpu[];
  primary_gpu: number;
  display: { width: number; height: number; refresh_hz: number; vrr: boolean | null; hdr: boolean | null };
  power: { on_ac: boolean | null; battery_percent: number | null; plan: string | null };
  hags: boolean | null;
  device: {
    manufacturer: string;
    model: string;
    form: "laptop" | "handheld" | "desktop" | "unknown";
    profile_hint: string | null;
  };
  vc_redist: boolean;
  webview2: string | null;
  free_disk_gb: number;
}

export type ToggleValue = boolean | number | string;

export interface Toggle {
  id: string;
  label: string;
  help: string;
  value: ToggleValue;
  /** The profile's own value; Apply sends only the toggles whose value differs from it. */
  default: ToggleValue;
  kind: "bool" | "choice" | "number";
  choices?: string[];
  experimental?: boolean;
  /** Proposed: bounds for kind "number". The UI falls back to a plain number field without them. */
  min?: number;
  max?: number;
  step?: number;
  unit?: string;
}

export interface Profile {
  id: string;
  name: string;
  description: string;
  recommended: boolean;
  toggles: Toggle[];
}

export type Overrides = Record<string, string | number | boolean>;

export interface InstalledVersion {
  version: string;
  channel: string;
  path: string;
  installed_at: string;
}

export interface LauncherState {
  install_dir: string;
  user_data_dir: string;
  installed: InstalledVersion[];
  current: string | null;
  profile_id: string | null;
  first_run: boolean;
  keys_present: boolean;
  firmware_present: boolean;
  game_folders: string[];
}

export type Channel = "stable" | "nightly";

export interface UpdateInfo {
  current: string | null;
  latest: string;
  channel: string;
  published_at: string;
  notes_md: string;
  size_bytes: number;
  sha256: string;
  update_available: boolean;
}

export interface InstallResult {
  version: string;
  path: string;
  verified_sha256: boolean;
  seconds: number;
}

export type Container = "nup" | "nsp" | "xci" | "nx" | "other";

export interface Game {
  title_id: string;
  title: string;
  path: string;
  container: Container;
  size_bytes: number;
  is_ocarina: boolean;
}

export interface FoundKey {
  path: string;
  kind: "prod" | "title" | "unknown";
  source_hint: string;
}

export interface FoundFirmware {
  path: string;
  version: string | null;
  source_hint: string;
}

export interface Found {
  keys: FoundKey[];
  firmware: FoundFirmware[];
  games: Game[];
  scanned_dirs: number;
  seconds: number;
}

export interface ImportResult {
  ok: boolean;
  dest: string;
  message: string;
}

export interface AppliedProfile {
  profile_id: string;
  switches_path: string;
  config_path: string;
  changed: string[];
}

export type CheckStatus = "ok" | "warn" | "fail" | "info";

export interface Check {
  id: string;
  title: string;
  status: CheckStatus;
  detail: string;
  fix_id: string | null;
  fix_label: string | null;
}

/** Payload of locate://progress, doctor://progress and install://progress. */
export interface Progress {
  stage: string;
  done: number;
  total: number;
  message?: string;
}

export type ReportKind = "bug" | "performance" | "crash";

export interface Report {
  zip_path: string;
  issue_url: string;
  summary_md: string;
}

export interface LaunchArgs {
  title_id?: string;
  path?: string;
  profile_id?: string;
}

/** Payload of game://exited. */
export interface GameExited {
  code: number;
  seconds: number;
}

/** Payload of warm://progress (warm_shaders, and the automatic run after a GPU driver change). */
export interface WarmProgress {
  title_id: string;
  stage: "ready" | "boot-builds" | "precache" | "retry" | "done";
  done: number;
  total: number;
  pass: number;
  percent: number;
  seconds: number;
  auto: boolean;
  message?: string;
}

export interface WarmResult {
  title_id: string;
  status: "done" | "incomplete" | "stopped" | "nothing built" | "nothing to do" | "failed";
  seconds: number;
  built: number;
  already: number;
  left: number;
  /** 0 ok; 1 configuration problem; 3 nothing built even after a second run; 4 stopped or pipelines left: run again. */
  exit_code: number | null;
  message: string | null;
}

export type ControllerKind = "xbox" | "playstation" | "switch" | "generic";

export interface Controller {
  id: string;
  name: string;
  kind: ControllerKind;
  connected: boolean;
  battery: number | null;
}

// ---- Modes, community data and the autopilot ---------------------------------------------------------

export type ModeId = "auto" | "smooth" | "quality" | "battery";

export interface AutopilotChange {
  id: number;
  at: string;
  mode: string;
  /** "change" can be undone; "action" is a shader warm-up; "note" is something kept. */
  kind: "change" | "action" | "note";
  setting: string;
  from: ToggleValue;
  to: ToggleValue;
  because: string;
  /** "Autopilot changed X from A to B because Y." */
  text: string;
  title_id?: string | null;
  undone: boolean;
}

export interface AutopilotView {
  enabled: boolean;
  history: AutopilotChange[];
  can_undo: boolean;
  latest: AutopilotChange | null;
}

export interface CommunityView {
  class: string;
  label: string;
  samples: number;
  validated: number;
  min_validated: number;
  source: "builtin" | "cache" | "network";
  used: boolean;
  applied: string[];
}

export interface ModeStatus {
  /** "custom" after a manual Apply under Advanced. */
  mode: ModeId | "custom";
  profile_id: string;
  profile_name: string;
  /** One sentence: what the mode chose and why. */
  reason: string;
  overrides: Overrides;
  community: CommunityView;
  autopilot: AutopilotView;
  note: string | null;
}

export interface StopResult {
  was_running: boolean;
  title_id: string | null;
  stopped: boolean;
  message: string;
}

export type AiService = "claude" | "chatgpt";

export interface AskAiResult {
  service: AiService;
  url: string;
  prompt: string;
  url_chars: number;
}

export interface CreditPerson {
  /** GitHub login; null for someone only known by the name in a commit trailer. */
  login: string | null;
  name: string | null;
  commits: number;
  coauthored: number;
}

export interface CreditTool {
  name: string;
  commits: number;
}

/** The hall of fame (get_credits): people first, AI tools and bots apart. */
export interface Credits {
  as_of: string | null;
  commits: number;
  people: CreditPerson[];
  tools: CreditTool[];
  source: "builtin" | "cache" | "network";
}

/** "Share my numbers": one finished session, read from the emulator's log. */
export interface ShareOffer {
  at: string;
  title_id: string | null;
  game: string;
  game_fps: number;
  within2: number;
  shown_fps: number;
  p50: number;
  p95: number;
  p99: number;
  gameplay_secs: number;
  fluid: boolean;
  on_battery: boolean;
  mode: string;
  profile: string;
  build: string;
  /** "Your session: 25 game fps, 93% of frames on time. Share it with the community?" */
  text: string;
}

export interface ShareStatus {
  /** False after "Don't ask again". */
  enabled: boolean;
  offer: ShareOffer | null;
}

export interface ShareLink {
  url: string;
  url_chars: number;
  /** "full", "short" (essentials only) or "clipboard" (an empty form; paste the text). */
  level: "full" | "short" | "clipboard";
  /** Every field as plain text, for the clipboard. */
  text: string;
}
