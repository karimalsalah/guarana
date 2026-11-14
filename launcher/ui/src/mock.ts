// Mock backend for previews in a plain browser (no window.__TAURI_INTERNALS__). It is stateful and
// streams the same events as the Rust side, with the profiles, check ids, fix ids and stage names
// of launcher/CONTRACT.md and src-tauri.
//
// Query parameters:
//   mock=first-run (default) | ready | no-oot | failure
//   keys=0|1  update=0|1  first=0|1  pad=playstation|xbox|switch|generic|none|late
//   crash=1 (the game exits with an error right after it starts)  session=<s> (seconds until game://exited)
//   warm=1 (an automatic shader warm-up after a driver change runs when the app opens)
//   open=<title id> (the app was opened with guarana://launch/<title id>; an id not in the library is refused)
//   speed=<factor> (0.25 makes every delay four times shorter, 20 lets you watch each stage)
//   boot=session (default) | old | none (the game://boot events a launch plays: a launcher session,
//     an older emulator whose window only takes the foreground, or nothing at all)
//   share=0|1 (a finished session's numbers, the "Share my numbers" offer, are waiting; default 1,
//     0 for first-run. With 0 the sample PC has never finished a session, and the numbers the UI
//     kept from an earlier visit are forgotten too)

import type { Backend, Unlisten } from "./api";
import type {
  AppliedProfile, AskAiResult, AutopilotChange, BootProgress, Credits, Channel, ModeStatus, StopResult, Check, Controller, ControllerKind, Found, FoundKey, Game, GameExited, ImportResult,
  InstallResult, InstalledVersion, LauncherState, Overrides, Profile, Progress, Report, ReportKind, SystemInfo,
  Toggle, ToggleValue, UpdateInfo, WarmProgress, WarmResult, ShareStatus, ShareLink, LaunchRequest, Notifications, BoosterStatus, BoosterLevelInfo,
} from "./types";

type PadMode = ControllerKind | "none" | "late";

interface Scenario {
  name: string;
  firstRun: boolean;
  keys: boolean;
  keysOnDisk: "quick" | "deep";
  firmware: boolean;
  oot: boolean;
  library: boolean;
  update: boolean;
  pad: PadMode;
  fail: boolean;
  crash: boolean;
  autoWarm: boolean;
  open: string | null;
  session: number;
  speed: number;
  boot: "session" | "old" | "none";
  /** `?share=`: a finished session's numbers wait in the share offer. */
  share: boolean;
  /** `?mock=contaminated`: the settings-hygiene rows of the doctor find something (issue #125). */
  contaminated: boolean;
}

const HOME = "C:\\Users\\Player";
const GAME_DIR = "D:\\Games\\Switch 2";
const DUMPS = `${HOME}\\Documents\\Switch 2 dumps`;
const KEY_BACKUP = "E:\\Backups\\console";
const OOT_ID = "0400486024DEC000";

function readScenario(): Scenario {
  const q = new URLSearchParams(location.search);
  const name = q.get("mock") || "first-run";
  const base: Scenario = {
    name,
    firstRun: true,
    keys: false,
    keysOnDisk: "deep",
    firmware: false,
    oot: true,
    library: false,
    update: true,
    pad: "playstation",
    fail: false,
    crash: false,
    autoWarm: false,
    open: q.get("open"),
    session: 7,
    speed: 1,
    boot: "session",
    share: name !== "first-run",
    contaminated: name === "contaminated",
  };
  if (name === "ready" || name === "no-oot" || name === "failure" || name === "contaminated") {
    Object.assign(base, { firstRun: false, keys: true, keysOnDisk: "quick", firmware: true, library: true });
  }
  if (name === "no-oot") Object.assign(base, { oot: false, update: false });
  if (name === "failure") Object.assign(base, { fail: true, pad: "xbox" });

  const flag = (k: string, d: boolean) => (q.has(k) ? q.get(k) !== "0" : d);
  base.keys = flag("keys", base.keys);
  base.update = flag("update", base.update);
  base.firstRun = flag("first", base.firstRun);
  base.crash = flag("crash", base.crash);
  base.autoWarm = flag("warm", base.autoWarm);
  base.share = flag("share", base.share);
  const pad = q.get("pad");
  if (pad === "playstation" || pad === "xbox" || pad === "switch" || pad === "generic" || pad === "none" || pad === "late") {
    base.pad = pad;
  }
  const session = Number(q.get("session"));
  if (session > 0) base.session = session;
  const speed = Number(q.get("speed"));
  if (speed > 0) base.speed = speed;
  const boot = q.get("boot");
  if (boot === "session" || boot === "old" || boot === "none") base.boot = boot;
  return base;
}

const clone = <T>(v: T): T => JSON.parse(JSON.stringify(v)) as T;
const daysAgo = (d: number) => new Date(Date.now() - d * 86_400_000).toISOString();

const SYSTEM: SystemInfo = {
  os: { name: "Windows 11 Home", build: 26100, display_version: "24H2" },
  cpu: { name: "Intel Core Ultra 9 285H", cores: 16, threads: 16, avx2: true, avx512: false, hybrid: true },
  ram_gb: 32,
  pagefile_gb: 5,
  gpus: [
    {
      name: "Intel(R) Arc(TM) 140T GPU (16GB)",
      vendor: "intel",
      driver_version: "32.0.101.6559",
      is_integrated: true,
      dedicated_mb: 128,
      shared_mb: 16125,
      vulkan: true,
      arch_hint: "Xe-LPG+ (Arrow Lake-H)",
      family: "Arrow Lake-H Intel Graphics",
    },
  ],
  primary_gpu: 0,
  display: { width: 2880, height: 1800, refresh_hz: 120, vrr: true, hdr: true },
  power: { on_ac: true, battery_percent: 86, plan: "Balanced" },
  hags: true,
  device: { manufacturer: "ASUS", model: "Zenbook 14 OLED (UX3405CA)", form: "laptop", profile_hint: null },
  vc_redist: true,
  webview2: "142.0.3595.53",
  free_disk_gb: 214.6,
};

// ---- Profiles: the same ids, names, toggles and defaults as src-tauri/src/profiles.rs ----------------

interface Defaults {
  frame_generation: boolean;
  ocarina_extrapolation: boolean;
  gm_routing: boolean;
  igpu_power_policy: boolean;
  async_shaders: boolean;
  vram_mode: string;
  resolution: string;
  fps_cap: string;
  fluid_presenter: boolean;
  fluid_experimental: boolean;
  hdr_auto: boolean;
}

const PROFILE_DEFS: { id: string; name: string; description: string; d: Defaults }[] = [
  {
    id: "intel-arc-igpu",
    name: "Intel Arc integrated",
    description: "Arc 130T/140T/140V and Meteor Lake or Panther Lake Arc graphics: asynchronous shaders, aggressive VRAM use, frame generation off (Ocarina of Time's own preset handles it).",
    d: { frame_generation: false, ocarina_extrapolation: true, gm_routing: false, igpu_power_policy: false, async_shaders: true, vram_mode: "aggressive", resolution: "1x", fps_cap: "off", fluid_presenter: true, fluid_experimental: false, hdr_auto: true },
  },
  {
    id: "amd-igpu-handheld",
    name: "AMD Radeon integrated",
    description: "Radeon 780M, 880M and 890M, Ryzen Z1 and Z2 handhelds: asynchronous shaders, conservative VRAM use.",
    d: { frame_generation: false, ocarina_extrapolation: true, gm_routing: false, igpu_power_policy: false, async_shaders: true, vram_mode: "conservative", resolution: "1x", fps_cap: "off", fluid_presenter: true, fluid_experimental: true, hdr_auto: true },
  },
  {
    id: "low-power-handheld",
    name: "Low power (on battery)",
    description: "Any integrated GPU on battery: a 30 fps cap and 0.75x resolution for steadier frames and longer play.",
    d: { frame_generation: false, ocarina_extrapolation: true, gm_routing: false, igpu_power_policy: false, async_shaders: true, vram_mode: "conservative", resolution: "0.75x", fps_cap: "30", fluid_presenter: false, fluid_experimental: true, hdr_auto: true },
  },
  {
    id: "desktop-dgpu",
    name: "Dedicated GPU",
    description: "A desktop or laptop graphics card with its own memory: asynchronous shaders, aggressive VRAM use.",
    d: { frame_generation: false, ocarina_extrapolation: true, gm_routing: false, igpu_power_policy: false, async_shaders: true, vram_mode: "aggressive", resolution: "1x", fps_cap: "off", fluid_presenter: false, fluid_experimental: true, hdr_auto: true },
  },
  {
    id: "safe-defaults",
    name: "Safe defaults",
    description: "The emulator's own defaults and no experiments: the profile to compare against when something looks wrong.",
    d: { frame_generation: false, ocarina_extrapolation: true, gm_routing: false, igpu_power_policy: false, async_shaders: false, vram_mode: "conservative", resolution: "1x", fps_cap: "off", fluid_presenter: false, fluid_experimental: true, hdr_auto: false },
  },
];

function toggles(d: Defaults): Toggle[] {
  const b = (id: string, label: string, help: string, value: boolean, experimental = false): Toggle => ({ id, label, help, value, default: value, kind: "bool", ...(experimental ? { experimental } : {}) });
  const c = (id: string, label: string, help: string, value: string, choices: string[]): Toggle => ({ id, label, help, value, default: value, kind: "choice", choices });
  return [
    b("frame_generation", "Frame generation", "Shows generated frames between the game's own: 3x up to 60 fps. Costs GPU time, so it suits games that already run near 20 fps or more.", d.frame_generation),
    b("ocarina_extrapolation", "Ocarina of Time: predict frames forward", "With frame generation on, Ocarina of Time's generated frames are predicted from the game's motion vectors instead of interpolated, adding no latency (EDEN_NX2_EXTRAPOLATE).", d.ocarina_extrapolation),
    b("gm_routing", "Global-memory routing in every game", "Cheaper global memory access in Switch 2 shaders (EDEN_NX2_GM=11). Ocarina of Time uses it already; this turns it on for every game.", d.gm_routing),
    b("igpu_power_policy", "Give the power budget to the GPU", "While a game runs, the emulator turns processor boost off in the active power plan so an integrated GPU gets more of the shared power, and restores the plan at exit (EDEN_NX2_POWER_POLICY=igpu).", d.igpu_power_policy, true),
    b("fluid_presenter", "Even frame pacing", "Shows frames on a steady vblank clock (16.7 ms apart at 60 Hz) instead of as soon as each is ready. Measured on an Arc iGPU: the fastest 5% of frame times went from about 11 ms to 16.4 ms, so motion looks even (EDEN_NX2_FLUID=1).", d.fluid_presenter, d.fluid_experimental),
    b("hdr_auto", "HDR when Windows HDR is on", "Uses the game's native HDR when Windows HDR is on (Win+Alt+B). Does nothing otherwise.", d.hdr_auto),
    b("async_shaders", "Asynchronous shaders", "Builds shaders in the background instead of pausing the game. Less stutter on first sight of new effects, which can be missing for a moment.", d.async_shaders),
    c("vram_mode", "Video memory use", "Aggressive keeps more textures and buffers in GPU memory: faster when there is room, more page-file use when there is not.", d.vram_mode, ["conservative", "aggressive"]),
    c("resolution", "Resolution", "Render resolution relative to the game's own. Below 1x saves GPU time on small screens.", d.resolution, ["0.5x", "0.75x", "1x", "1.25x", "1.5x", "2x"]),
    c("fps_cap", "Frame rate cap", "Caps how often the game presents a frame. A cap the GPU can always hold gives steadier frame times and saves power.", d.fps_cap, ["off", "30", "40", "45", "60"]),
  ];
}

// Any Intel GPU the launcher calls integrated, primary or not (profiles.rs `has_intel_igpu`): there the
// launcher leaves EDEN_NX2_EXTRAPOLATE to the emulator, whose locked 120 and 144 Hz sets for Ocarina of
// Time interpolate (src/core/game_presets.cpp `ChoosePacingSet`, issue #119).
function hasIntelIgpu(sys: SystemInfo): boolean {
  return sys.gpus.some((g) => g.vendor === "intel" && g.is_integrated);
}

// Appended to the `ocarina_extrapolation` help there (profiles.rs `INTEL_IGPU_HELP`).
const INTEL_IGPU_HELP =
  " With an Intel integrated GPU the launcher leaves EDEN_NX2_EXTRAPOLATE to the emulator when this is on: Ocarina of Time still predicts at most refresh rates, but at 115-125 or 140-148 Hz the emulator's 120 or 144 Hz set shows a frame for every refresh, the in-between ones interpolated, and a 1 in your nx2_switches.txt would turn that off. Older emulator builds predict there too (the shipped nx2_switches.txt sets 1).";

function buildGames(withOot: boolean): Game[] {
  const games: Game[] = [
    { title_id: "040074A01BF12000", title: "Donkey Kong Bananza", path: `${GAME_DIR}\\Donkey Kong Bananza.xci`, container: "xci", size_bytes: 10_955_210_752, is_ocarina: false },
    { title_id: "0400C3F00006E000", title: "Mario Kart World", path: `${GAME_DIR}\\Mario Kart World.nsp`, container: "nsp", size_bytes: 22_978_592_768, is_ocarina: false },
    { title_id: "040077201BE50000", title: "Kirby Air Riders", path: `${GAME_DIR}\\Kirby Air Riders.nsp`, container: "nsp", size_bytes: 9_234_212_864, is_ocarina: false },
    { title_id: "0400A3A01EE4C000", title: "Pokémon Pokopia", path: `${GAME_DIR}\\Pokopia.nsp`, container: "nsp", size_bytes: 7_516_192_768, is_ocarina: false },
  ];
  if (withOot) {
    games.unshift({
      title_id: OOT_ID, title: "The Legend of Zelda: Ocarina of Time",
      path: `${GAME_DIR}\\Ocarina of Time.nup`, container: "nup", size_bytes: 8_482_598_912, is_ocarina: true,
    });
  }
  return games;
}

const NOTES_STABLE = `## Guarana v20.1.0

### Faster
- **Global-memory routing** is on by default for Ocarina of Time. Measured on an Arc 140T in Hyrule Field: 14.75 → 22.83 game fps, frame generation off.
- Shader cache files are read ahead when you press Play, so the first minute hitches less.

### Fixed
- Frame generation no longer smears the HUD while the camera turns.
- Intel drivers older than \`32.0.101.9175\` get a warning in Doctor instead of a crash at boot.

### Known issues
- The first visit to an area still hitches while shaders compile.

Full notes: https://github.com/karimalsalah/guarana/releases`;

const NOTES_NIGHTLY = `## Nightly 2026-11-13

Built from \`main\` at 838951d. Nightlies change every day and can break.

- ci: publish the platforms that built (Windows required)
- build: include \`<cstdint>\` before the generated extrapolation shader header
- shader_recompiler: \`EDEN_NX2_GM=11\` on by default for Ocarina of Time only

> Your current version stays installed, so you can roll back in one click.`;

const NOTES_CURRENT = `## Guarana v20.0.2

- Release builds publish every platform that built; Windows is required.
- No changes to the emulator itself since v20.0.1.`;

export function createMockBackend(): Backend {
  const sc = readScenario();
  const listeners = new Map<string, Set<(payload: unknown) => void>>();
  const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms * sc.speed));
  const emit = (name: string, payload: unknown) => {
    for (const h of listeners.get(name) ?? []) h(clone(payload));
  };

  const allGames = buildGames(sc.oot);
  let library: Game[] = sc.library ? clone(allGames) : [];
  let shaderTmp = true;
  let driverPageOpened = false;
  let firstLaunch = true;
  let gameRunning = false;
  let bundledDone = false;
  const warming = new Map<string, number>();
  let stopWarm = false;
  // Modes and the autopilot (the backend's modes.rs and autopilot.rs, in short).
  let mode: ModeStatus["mode"] = "auto";
  let autopilotOn = true;
  // "Share my numbers": the mock offers one session until Share or Not now (share=0: none yet), and
  // a fresh one after each clean exit, like share.rs after a session with a minute of gameplay.
  let shareOn = sc.share;
  let sharePrompt = true;
  /** The sample sessions: the one waiting at start (three hours ago), then each one played here. */
  const SESSIONS = [
    { game_fps: 25.2, within2: 93.4, shown_fps: 49.6, p50: 16.7, p95: 33.4, p99: 100.1 },
    { game_fps: 26.8, within2: 96.1, shown_fps: 52.9, p50: 16.7, p95: 25.1, p99: 50.2 },
    { game_fps: 24.1, within2: 88.7, shown_fps: 47.3, p50: 16.7, p95: 41.7, p99: 116.8 },
  ];
  let sessionIndex = sc.share ? 0 : -1;
  let offerAt = new Date(Date.now() - 3 * 3_600_000).toISOString();
  let offerSecs = 1290;
  if (!sc.share) {
    // A PC that never finished a session: forget numbers the UI kept from an earlier sample visit.
    try {
      localStorage.removeItem("guarana.pacing");
    } catch {
      /* storage disabled: nothing kept anyway */
    }
  }
  let notifications = false;
  // guarana://launch/<id> (open=<id>): the backend's open.rs checks it against the library.
  const opened = sc.open ? allGames.find((g) => g.title_id.toLowerCase() === sc.open?.toLowerCase()) : undefined;
  let launchRequest: LaunchRequest | null = sc.open
    ? opened
      ? { source: "link", game: opened, error: null }
      : { source: "link", game: null, error: `No game with title id ${sc.open.toUpperCase()} in your library` }
    : null;
  // Game Booster (the backend's booster.rs, in short). Sample PC: High performance is installed
  // (not Ultimate), and SysMain and Windows Search are running.
  let boosterLevel = 0;
  let boosterActive = false;
  function boosterLevels(): BoosterLevelInfo[] {
    const out: BoosterLevelInfo[] = [];
    for (let level = 0; level <= 3; level++) {
      const changes: string[] = [];
      let elevated = false;
      let summary = "No changes. Windows runs games as usual.";
      if (level >= 1) {
        changes.push(`Run Guarana at ${level >= 3 ? "High" : "Above Normal"} priority`);
        changes.push("Turn off power throttling (EcoQoS) for Guarana");
        changes.push("Ask Windows for the high-performance GPU for guarana.exe");
        summary = "No admin needed: priority, power throttling and the GPU choice for Guarana.";
      }
      if (level >= 2) {
        changes.push("Switch the power plan to High performance for this session, then put it back");
        elevated = true;
        summary = "Above Normal priority, power throttling off and the high-performance GPU for Guarana, and one Windows permission prompt to switch the power plan to High performance.";
      }
      if (level >= 3) {
        changes.push("Pause SysMain and Windows Search while you play, and restart them after");
        elevated = true;
        summary = "High priority, power throttling off and the high-performance GPU for Guarana, and one Windows permission prompt to switch the power plan to High performance, and pause SysMain and Windows Search.";
      }
      out.push({ level, title: level === 0 ? "Off" : `Level ${level}`, summary, elevated, changes });
    }
    return out;
  }
  const boosterStatus = (note: string | null = null): BoosterStatus => ({ level: boosterLevel, active: boosterActive, levels: boosterLevels(), note });
  const shareStatus = (): ShareStatus => ({
    enabled: sharePrompt,
    offer:
      shareOn && sharePrompt
        ? (() => {
            const m = SESSIONS[Math.max(0, sessionIndex) % SESSIONS.length];
            return {
              at: offerAt,
              title_id: OOT_ID,
              game: "The Legend of Zelda: Ocarina of Time",
              ...m,
              gameplay_secs: offerSecs,
              fluid: true,
              on_battery: false,
              mode: "auto",
              profile: "intel-arc-igpu",
              build: "20.1.0 (stable)",
              text: `Your session: ${Math.round(m.game_fps)} game fps, ${Math.round(m.within2)}% of frames on time. Share it with the community?`,
            };
          })()
        : null,
  });
  const history: AutopilotChange[] = [];
  const MODE_TEXT: Record<string, string> = {
    auto: "Auto chose Intel Arc integrated for your Intel Arc 140T (plugged in), and set global-memory routing like the 6 validated reports for Intel Arc iGPU.",
    smooth: "Smooth mode builds on Intel Arc integrated for your Intel Arc 140T with even frame pacing on and no frame generation, trading a little resolution for steadier frames.",
    quality: "Quality mode builds on Intel Arc integrated for your Intel Arc 140T with one step more resolution and more video memory.",
    battery: "Battery mode runs Low power (on battery): 0.75x resolution and a 30 fps cap to save power.",
    custom: "Custom settings from Advanced. Pick a mode to hand control back.",
  };
  function modeStatus(note: string | null = null): ModeStatus {
    return {
      mode,
      profile_id: mode === "battery" ? "low-power-handheld" : state.profile_id ?? "intel-arc-igpu",
      profile_name: mode === "battery" ? "Low power (on battery)" : "Intel Arc integrated",
      reason: MODE_TEXT[mode],
      overrides: mode === "auto" ? { gm_routing: true } : {},
      community: { class: "intel-arc-igpu", label: "Intel Arc iGPU", samples: 9, validated: 6, min_validated: 3, source: "network", used: mode === "auto", applied: mode === "auto" ? ["gm_routing"] : [] },
      autopilot: {
        enabled: autopilotOn,
        history: [...history].reverse().slice(0, 10),
        can_undo: history.some((c) => c.kind === "change" && !c.undone),
        latest: [...history].reverse().find((c) => !c.undone) ?? null,
      },
      note,
    };
  }
  if (sc.oot && !sc.firstRun) {
    history.push({
      id: 1, at: new Date(Date.now() - 86_400_000).toISOString(), mode: "auto", kind: "change", setting: "resolution", from: "1x", to: "0.75x",
      because: "the game ran at 12 fps, under 15",
      text: "Autopilot changed the resolution from 1x to 0.75x because the game ran at 12 fps, under 15.", undone: false,
    });
  }
  const appliedOverrides: Record<string, Overrides> = {};

  const installed: InstalledVersion[] = [
    { version: "20.0.2", channel: "stable", path: `${HOME}\\AppData\\Local\\Guarana\\versions\\20.0.2`, installed_at: daysAgo(5) },
    { version: "20.0.1", channel: "stable", path: `${HOME}\\AppData\\Local\\Guarana\\versions\\20.0.1`, installed_at: daysAgo(9) },
    { version: "20.0.0", channel: "stable", path: `${HOME}\\AppData\\Local\\Guarana\\versions\\20.0.0`, installed_at: daysAgo(16) },
  ];

  const state: LauncherState = {
    install_dir: `${HOME}\\AppData\\Local\\Guarana`,
    user_data_dir: `${HOME}\\AppData\\Roaming\\guarana`,
    installed,
    current: "20.0.2",
    profile_id: sc.firstRun ? null : "intel-arc-igpu",
    first_run: sc.firstRun,
    keys_present: sc.keys,
    firmware_present: sc.firmware,
    game_folders: sc.library ? [GAME_DIR] : [],
  };

  let controllers: Controller[] = padList(sc.pad === "late" ? "none" : sc.pad);
  if (sc.pad === "late") {
    setTimeout(() => {
      controllers = padList("playstation");
      emit("controller://changed", controllers);
    }, 3500 * sc.speed);
  }

  function padList(kind: PadMode): Controller[] {
    switch (kind) {
      case "playstation":
        return [{ id: "hid:054c:0ce6", name: "DualSense Wireless Controller", kind, connected: true, battery: 72 }];
      case "xbox":
        return [{ id: "xinput:0", name: "Xbox Wireless Controller", kind, connected: true, battery: 55 }];
      case "switch":
        return [{ id: "hid:057e:2009", name: "Pro Controller", kind, connected: true, battery: 90 }];
      case "generic":
        return [{ id: "hid:2dc8:3106", name: "8BitDo Ultimate 2C", kind, connected: true, battery: null }];
      default:
        return [];
    }
  }

  function buildProfiles(): Profile[] {
    const intelIgpu = hasIntelIgpu(SYSTEM);
    return PROFILE_DEFS.map((p) => {
      const list = toggles(p.d);
      const predict = list.find((t) => t.id === "ocarina_extrapolation");
      if (intelIgpu && predict) predict.help += INTEL_IGPU_HELP;
      const over = appliedOverrides[p.id] ?? {};
      for (const t of list) if (t.id in over) t.value = over[t.id] as ToggleValue;
      return { id: p.id, name: p.name, description: p.description, recommended: p.id === "intel-arc-igpu", toggles: list };
    });
  }

  function keysFound(scope: "quick" | "deep"): FoundKey[] {
    if (sc.keys || sc.keysOnDisk === "quick" || scope === "deep") {
      const dir = sc.keysOnDisk === "quick" ? DUMPS : KEY_BACKUP;
      return [
        { path: `${dir}\\prod.keys`, kind: "prod", source_hint: dir },
        { path: `${dir}\\title.keys`, kind: "title", source_hint: dir },
      ];
    }
    return [];
  }

  function updateInfo(channel: Channel): UpdateInfo {
    if (channel === "nightly") {
      const latest = "nightly-2026-11-13";
      return {
        current: state.current, latest, channel, published_at: new Date(Date.now() - 6 * 3_600_000).toISOString(),
        notes_md: NOTES_NIGHTLY, size_bytes: 53_477_376, sha256: "4c1d0e9a7f3b52c86e1f0a9d2b7c4e6f8a1b3c5d7e9f0a2b4c6d8e0f1a3b5c7d",
        update_available: state.current !== latest,
      };
    }
    const latest = sc.update ? "20.1.0" : "20.0.2";
    return {
      current: state.current, latest, channel, published_at: sc.update ? "2026-11-10T16:00:00Z" : daysAgo(5),
      notes_md: sc.update ? NOTES_STABLE : NOTES_CURRENT, size_bytes: 51_234_816,
      sha256: "9f2c71d0b3e84a56c1d7e2f9a8b04c3d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b",
      update_available: state.current !== latest,
    };
  }

  // Checks in the contract's order: emulator_version … controller.
  function checks(): Check[] {
    const ok = (id: string, title: string, detail: string): Check => ({ id, title, status: "ok", detail, fix_id: null, fix_label: null });
    const updateOut = sc.update && state.current === "20.0.2";
    return [
      updateOut
        ? { id: "emulator_version", title: "Guarana version", status: "warn", detail: "v20.0.2 is installed; v20.1.0 is out.", fix_id: "install_update", fix_label: "Update to v20.1.0" }
        : ok("emulator_version", "Guarana version", `v${state.current ?? "?"} is the latest.`),
      driverPageOpened
        ? { id: "gpu_driver", title: "GPU driver", status: "warn", detail: "Intel 32.0.101.6559. The driver page is open in your browser: install 32.0.101.9175, then run the checks again.", fix_id: "open_driver_page", fix_label: "Open the page again" }
        : { id: "gpu_driver", title: "GPU driver", status: "warn", detail: "Intel 32.0.101.6559 is older than the tested 32.0.101.9175. Older drivers can crash while shaders build.", fix_id: "open_driver_page", fix_label: "Get the driver" },
      sc.fail
        ? { id: "vulkan", title: "Vulkan driver", status: "fail", detail: "No Vulkan driver answered. Reinstall the GPU driver.", fix_id: "open_vulkan_driver_page", fix_label: "Get the driver" }
        : ok("vulkan", "Vulkan driver", "Vulkan 1.4 on the Intel Arc 140T."),
      sc.fail
        ? { id: "vc_redist", title: "Visual C++ runtime", status: "fail", detail: "Microsoft Visual C++ 2015-2022 x64 is missing.", fix_id: "open_vc_redist", fix_label: "Get the runtime" }
        : ok("vc_redist", "Visual C++ runtime", "Microsoft Visual C++ 2015-2022 x64 is installed."),
      ok("avx2", "CPU features", "AVX2 is supported."),
      ok("memory", "Memory and page file", "32 GB RAM. The page file is system managed, so it can grow."),
      ok("memory_now", "Memory free right now", "17.6 GB of RAM is free, and 14.2 of the 37.0 GB Windows can hand out (RAM plus page file) is in use."),
      ok("disk", "Free disk space", "214 GB free on C: for shader caches."),
      { id: "power", title: "Power mode", status: "warn", detail: "Plugged in, Balanced. Best performance keeps the GPU clock up.", fix_id: "open_power_settings", fix_label: "Open power settings" },
      { id: "hags", title: "Hardware-accelerated GPU scheduling", status: "info", detail: "On. If you see stutter, try it off and compare.", fix_id: "open_graphics_settings", fix_label: "Open graphics settings" },
      { id: "intel_shared_memory", title: "Intel Shared GPU Memory Override", status: "info", detail: "Not set. Optional: Intel Graphics Software can give the iGPU more shared memory.", fix_id: "open_intel_graphics", fix_label: "Learn how" },
      { id: "windows_hdr", title: "Windows HDR", status: "info", detail: "This display can do HDR and Windows HDR is off. Turn it on with Win+Alt+B for the game's native HDR.", fix_id: "open_hdr_help", fix_label: "How to turn on HDR" },
      ok("refresh_rate", "Display refresh rate and frame generation", "120 Hz is a multiple of 60."),
      { id: "displays", title: "Displays on the integrated GPU", status: "warn", detail: "2 displays are on, all drawn by the Intel(R) Arc(TM) 140T GPU (16GB) that also runs the game. Each one takes memory bandwidth and compositor work from that GPU. If you do not need one while playing, turn it off in Display settings (select it, then Multiple displays > Disconnect this display).", fix_id: "open_display_settings", fix_label: "Open Display settings" },
      { id: "defender", title: "Windows Security and the cache folder", status: "info", detail: "The shader cache folder is scanned on every write. An exclusion can reduce hitches. Guarana never changes this for you.", fix_id: "open_windows_security", fix_label: "Open Windows Security" },
      shaderTmp
        ? { id: "shader_tmp", title: "Shader cache temp files", status: "warn", detail: "14 leftover temp files (38 MB) from an earlier crash.", fix_id: "delete_shader_tmp", fix_label: "Delete temp files" }
        : ok("shader_tmp", "Shader cache temp files", "No leftover temp files."),
      // Settings hygiene (issue #125): these rows only report, so none has a fix.
      sc.contaminated
        ? { id: "switch_file", title: "Switch file", status: "warn", detail: "The launcher's block in nx2_switches.txt is not what this launcher writes for your saved settings: EDEN_NX2_EXTRAPOLATE=1 (this launcher leaves it out). It comes from an older launcher: with an Intel integrated GPU, EDEN_NX2_EXTRAPOLATE=1 turns off the measured 120 and 144 Hz sets of Ocarina of Time (at 144 Hz, 58.5 frames shown a second instead of 91.5; issue #119). The launcher rewrites its block each time you start a game from it, so start your next game from the launcher; starting guarana.exe from its own shortcut keeps the old block until then. The launcher's block sets EDEN_NX2_FLUID=1 (Even frame pacing). Ocarina of Time has a measured value of its own, EDEN_NX2_FLUID=2, but a line in nx2_switches.txt wins over it, so Ocarina runs with 1. Nothing to do for now: a coming update lets the game's measured value win (issue #125). Turning Even frame pacing off under Home > Advanced gives Ocarina its own value but takes even pacing from your other games.", fix_id: null, fix_label: null }
        : ok("switch_file", "Switch file", "nx2_switches.txt reads as Guarana expects, the launcher's block is what this launcher writes, and no EDEN_NX2_ variable is set in Windows."),
      sc.contaminated
        ? { id: "settings_file", title: "Settings", status: "warn", detail: "qt-config.ini turns off \"Use persistent pipeline cache\" (use_disk_shader_cache=false): every start of a game compiles its shaders from nothing, and Prepare shaders has nothing to fill. Turn it back on in Guarana's Configure window, unless you turned it off on purpose to work around a driver problem.", fix_id: null, fix_label: null }
        : ok("settings_file", "Settings", "The shader cache settings are on, and the launcher's saved settings are all ones it knows."),
      sc.contaminated
        ? { id: "cache_files", title: "Shader cache files", status: "warn", detail: `Prepare shaders marked ${OOT_ID} as done, but the GPU driver's pipeline cache file (vulkan_pipelines.fork-v*.bin) is missing or empty, so the shaders it lists are not there and the first play compiles them (issue #106). Run Prepare shaders on the game card again to have them ready before you play.`, fix_id: null, fix_label: null }
        : ok("cache_files", "Shader cache files", "No shader cache marker or leftover needs attention. The installed Guarana does not say which cache format it writes (it has no build-facts.json), so the formats are not compared."),
      sc.contaminated
        ? { id: "user_folder", title: "Settings folder", status: "warn", detail: "Your Lichii settings may not have been carried over to Guarana (issue #107): %APPDATA%\\lichii\\config\\qt-config.ini has 14 setting(s) changed from their defaults and Guarana's own qt-config.ini has none. Guarana copies the lichii folder only on a start when no guarana folder exists yet, and never again after that. Nothing was changed: a coming launcher update offers to copy the settings over (never the folders where saves are kept). Until then, set them again in Guarana. %APPDATA%\\lichii also has a nand or sdmc folder, where Lichii kept saves; this check does not look inside them.", fix_id: null, fix_label: null }
        : ok("user_folder", "Settings folder", "Nothing was left behind in %APPDATA%\\lichii."),
      state.keys_present
        ? ok("keys", "Keys", "prod.keys is in Guarana's keys folder.")
        : { id: "keys", title: "Keys", status: "fail", detail: "No prod.keys yet. Dump it from your console, then import it under Your files.", fix_id: null, fix_label: null },
      state.firmware_present
        ? ok("firmware", "Firmware", "Firmware 20.5.0 is installed.")
        : { id: "firmware", title: "Firmware", status: "warn", detail: "Not installed yet. Games need firmware dumped from your console.", fix_id: null, fix_label: null },
      state.game_folders.length
        ? ok("game_folder", "Game folder", `${state.game_folders.length} folder: ${state.game_folders.join(", ")}`)
        : { id: "game_folder", title: "Game folder", status: "warn", detail: "No game folder yet.", fix_id: null, fix_label: null },
      library.some((g) => g.is_ocarina)
        ? ok("ocarina", "Ocarina of Time", `Found in ${GAME_DIR}.`)
        : { id: "ocarina", title: "Ocarina of Time", status: "info", detail: "Not in your library. Its profile and motion data are ready when you add it.", fix_id: null, fix_label: null },
      controllers.length
        ? ok("controller", "Controller", `${controllers[0].name} is connected.`)
        : { id: "controller", title: "Controller", status: "info", detail: "None connected. Keyboard works; pair a controller in Bluetooth settings.", fix_id: "open_bluetooth_settings", fix_label: "Open Bluetooth settings" },
    ];
  }

  async function locate(scope: "quick" | "deep"): Promise<Found> {
    // done/total count roots, as the backend does; message is the root being searched.
    const roots = scope === "quick"
      ? [`${HOME}\\Downloads`, `${HOME}\\Documents`, `${HOME}\\Desktop`, GAME_DIR]
      : ["C:\\", "D:\\", "E:\\"];
    for (let i = 0; i < roots.length; i++) {
      emit("locate://progress", { stage: "scan", done: i, total: roots.length, message: roots[i] } satisfies Progress);
      await sleep(scope === "quick" ? 480 : 1500);
    }
    emit("locate://progress", { stage: "done", done: roots.length, total: roots.length } satisfies Progress);
    return {
      keys: keysFound(scope),
      firmware: sc.firmware ? [] : [{ path: `${DUMPS}\\firmware 20.5.0`, version: "20.5.0", source_hint: DUMPS }],
      games: clone(allGames),
      scanned_dirs: scope === "quick" ? 412 : 18_432,
      seconds: scope === "quick" ? 2.1 : 5.2,
    };
  }

  async function install(version: string, channel: string): Promise<InstallResult> {
    const size = version.includes("nightly") ? 53_477_376 : 51_234_816;
    const p = (stage: string, done: number, total: number, message?: string) => emit("install://progress", { stage, done, total, message } satisfies Progress);
    p("resolve", 0, 1, "GitHub Releases");
    await sleep(500);
    const steps = 24;
    for (let i = 1; i <= steps; i++) {
      await sleep(140 + Math.random() * 90);
      p("download", Math.round((size * i) / steps), size, `guarana-${version}-windows-x86_64.zip`);
    }
    p("verify", 0, 1, "SHA-256");
    await sleep(700);
    if (sc.fail) throw "The SHA-256 of the download didn't match the release. Nothing was installed; try again.";
    for (let i = 1; i <= 6; i++) {
      await sleep(180);
      p("extract", i * 214, 1284);
    }
    p("done", 1, 1);
    const path = `${HOME}\\AppData\\Local\\Guarana\\versions\\${version}`;
    if (!installed.some((v) => v.version === version)) installed.unshift({ version, channel, path, installed_at: new Date().toISOString() });
    return { version, path, verified_sha256: true, seconds: 6.4 };
  }

  async function warm(titleId: string, auto: boolean): Promise<WarmResult> {
    if (gameRunning) throw "Shaders can't be prepared while the game runs. Close it first.";
    if (warming.has(titleId)) throw `Shaders for this game are already being prepared (${warming.get(titleId)}%).`;
    const total = 1840;
    const started = Date.now();
    const p = (stage: WarmProgress["stage"], percent: number, pass: number, message?: string) => {
      warming.set(titleId, percent);
      emit("warm://progress", { title_id: titleId, stage, done: Math.round((total * percent) / 100), total, pass, percent, seconds: (Date.now() - started) / 1000, auto, message } satisfies WarmProgress);
    };
    try {
      p("ready", 0, 1);
      await sleep(600);
      for (let pct = 4; pct <= 100; pct += 4) {
        await sleep(260);
        if (stopWarm) {
          stopWarm = false;
          p("done", pct, 1, "stopped");
          return { title_id: titleId, status: "stopped", seconds: (Date.now() - started) / 1000, built: Math.round(total * pct / 100), already: 0, left: total - Math.round(total * pct / 100), exit_code: 4, message: "Stopped" };
        }
        p(pct < 30 ? "boot-builds" : "precache", pct, 1);
      }
      p("done", 100, 1);
      return { title_id: titleId, status: "done", seconds: (Date.now() - started) / 1000, built: 1712, already: 128, left: 0, exit_code: 0, message: null };
    } finally {
      warming.delete(titleId);
    }
  }

  if (sc.autoWarm && sc.oot) setTimeout(() => void warm(OOT_ID, true).catch(() => undefined), 2500 * sc.speed);

  // Commands reject with a readable string, as the contract says.
  const commands: Record<string, (a: Record<string, unknown>) => Promise<unknown>> = {
    async detect_system() {
      await sleep(650);
      return SYSTEM;
    },
    async list_profiles() {
      await sleep(220);
      return buildProfiles();
    },
    async get_state() {
      // First start: the backend installs the build the installer carries before answering.
      if (sc.firstRun && !bundledDone) {
        bundledDone = true;
        const p = (stage: string, done: number, total: number, message?: string) => emit("install://progress", { stage, done, total, message } satisfies Progress);
        await sleep(300);
        p("verify", 0, 1, "Checking the bundled build");
        await sleep(500);
        p("verify", 1, 1);
        for (let i = 1; i <= 8; i++) {
          await sleep(160);
          p("extract", i * 160, 1284);
        }
      }
      await sleep(90);
      return state;
    },
    async check_updates(a) {
      // Without a channel, the installed version's channel (as the backend does).
      await sleep(sc.fail ? 900 : 700);
      if (sc.fail) throw "Couldn't reach GitHub (timed out after 10 s). Check your connection and try again.";
      const cur = installed.find((v) => v.version === state.current);
      const asked = a.channel;
      const channel: Channel = asked === "nightly" || asked === "stable" ? asked : cur?.channel === "nightly" ? "nightly" : "stable";
      return updateInfo(channel);
    },
    async install_version(a) {
      return install(String(a.version), a.channel === "nightly" ? "nightly" : "stable");
    },
    async set_current_version(a) {
      await sleep(350);
      const version = String(a.version);
      if (!installed.some((v) => v.version === version)) throw `v${version} is not installed.`;
      state.current = version;
      return state;
    },
    async locate_files(a) {
      return locate(a.scope === "deep" ? "deep" : "quick");
    },
    async import_keys(a) {
      await sleep(600);
      const path = String(a.path);
      const name = path.split("\\").pop() ?? "keys";
      if (/prod\.keys$/i.test(path)) state.keys_present = true;
      const r: ImportResult = { ok: true, dest: `${state.user_data_dir}\\keys\\${name}`, message: `Copied ${name} into Guarana's keys folder.` };
      return r;
    },
    async add_game_folder(a) {
      await sleep(500);
      const path = String(a.path).replace(/\\+$/, "");
      if (!state.game_folders.includes(path)) state.game_folders.push(path);
      const inFolder = allGames.filter((g) => g.path.toLowerCase().startsWith(path.toLowerCase() + "\\"));
      for (const g of inFolder) if (!library.some((x) => x.title_id === g.title_id)) library.push(clone(g));
      return library;
    },
    async list_games() {
      await sleep(160);
      return library;
    },
    async apply_profile(a) {
      await sleep(520);
      if (gameRunning || warming.size) throw "The emulator or a shader warm-up is running. Your choice is saved and applied when it ends.";
      const id = String(a.profile_id ?? "");
      const def = PROFILE_DEFS.find((p) => p.id === id);
      if (!def) throw `Unknown profile "${id}".`;
      const overrides = (a.overrides ?? {}) as Overrides;
      appliedOverrides[id] = overrides;
      mode = "custom";
      state.profile_id = id;
      state.first_run = false;
      const r: AppliedProfile = {
        profile_id: id,
        switches_path: `${state.user_data_dir}\\nx2_switches.txt`,
        config_path: `${state.user_data_dir}\\config\\qt-config.ini`,
        changed: toggles(def.d).map((t) => t.id),
      };
      return r;
    },
    async run_doctor() {
      const list = checks();
      for (const c of list) {
        emit("doctor://progress", { stage: c.id, done: list.indexOf(c), total: list.length, message: c.title } satisfies Progress);
        await sleep(90);
      }
      emit("doctor://progress", { stage: "done", done: list.length, total: list.length } satisfies Progress);
      return list;
    },
    async apply_fix(a) {
      const fix = String(a.fix_id);
      if (fix === "install_update") {
        const r = await install("20.1.0", "stable");
        state.current = r.version;
      } else {
        await sleep(650);
        if (fix === "delete_shader_tmp") shaderTmp = false;
        if (fix === "open_driver_page") driverPageOpened = true;
      }
      const ids: Record<string, string> = { install_update: "emulator_version", delete_shader_tmp: "shader_tmp" };
      const updated = checks().find((c) => c.fix_id === fix || c.id === ids[fix]);
      if (!updated) throw `Unknown fix "${fix}".`;
      return updated;
    },
    async prefetch_caches(a) {
      await sleep(2600);
      if (!a.title_id) throw "title_id is required";
      return { bytes_warmed: 431_801_344 };
    },
    async warm_shaders(a) {
      return warm(String(a.title_id), false);
    },
    async stop_warm_shaders() {
      await sleep(300);
      if (!warming.size) return { was_running: false, title_id: null, stopped: false, message: "No shader preparation is running." } satisfies StopResult;
      stopWarm = true;
      const id = [...warming.keys()][0];
      while (warming.has(id)) await sleep(100);
      return { was_running: true, title_id: id, stopped: true, message: "Stopped. Preparing this game again later finishes the job; it may start with a short repair pass." } satisfies StopResult;
    },
    async pick_folder() {
      await sleep(400);
      return GAME_DIR;
    },
    async get_credits() {
      await sleep(250);
      return {
        as_of: "2026-11-20",
        commits: 27,
        people: [
          { login: "karimalsalah", name: null, commits: 16, coauthored: 0 },
          { login: "zortos293", name: null, commits: 11, coauthored: 1 },
          { login: null, name: "Julius Bairaktaris", commits: 0, coauthored: 1 },
        ],
        tools: [{ name: "Claude", commits: 17 }, { name: "Capy", commits: 7 }],
        source: "builtin",
      } satisfies Credits;
    },
    async get_mode() {
      await sleep(200);
      return modeStatus();
    },
    async set_mode(a) {
      await sleep(350);
      const id = String(a.mode);
      if (!["auto", "smooth", "quality", "battery"].includes(id)) throw `Unknown mode ${id}: use auto, smooth, quality or battery`;
      mode = id as ModeStatus["mode"];
      state.profile_id = mode === "battery" ? "low-power-handheld" : "intel-arc-igpu";
      state.first_run = false;
      for (const c of history) if (c.mode !== mode) c.undone = c.undone;
      return modeStatus(gameRunning ? "Saved. It is written when Guarana closes." : null);
    },
    async autopilot_undo() {
      await sleep(250);
      const c = [...history].reverse().find((x) => x.kind === "change" && !x.undone);
      if (!c) throw "Nothing to undo";
      c.undone = true;
      return modeStatus();
    },
    async autopilot_enable(a) {
      await sleep(150);
      autopilotOn = a.enabled === true;
      return modeStatus();
    },
    async get_share_status() {
      return shareStatus();
    },
    async share_open() {
      const text = ["device: ASUS Zenbook S 14", "results: Average 25.2 fps (game fps), 93% of shown frames within 2 ms of the median"].join(String.fromCharCode(10));
      return { url: "https://github.com/karimalsalah/guarana/issues/new?template=device-report.yml&title=%5BDevice%5D%20mock", url_chars: 90, level: "full", text } satisfies ShareLink;
    },
    async share_dismiss() {
      shareOn = false;
      return shareStatus();
    },
    async take_launch_request() {
      const r = launchRequest;
      launchRequest = null;
      return r;
    },
    async get_notifications() {
      return { enabled: notifications } satisfies Notifications;
    },
    async set_notifications(a) {
      notifications = a.enabled === true;
      if (notifications) console.info("[mock] notification: Notifications are on");
      return { enabled: notifications } satisfies Notifications;
    },
    async share_set_prompt(a) {
      sharePrompt = a.enabled === true;
      shareOn = shareOn && sharePrompt;
      return shareStatus();
    },
    async get_booster() {
      await sleep(140);
      return boosterStatus();
    },
    async set_booster_level(a) {
      await sleep(200);
      const level = Number(a.level);
      if (!Number.isInteger(level) || level < 0 || level > 3) throw `Unknown booster level ${a.level}: use 0 (Off) to 3`;
      boosterLevel = level;
      return boosterStatus();
    },
    async booster_undo_all() {
      await sleep(220);
      const was = boosterActive;
      boosterActive = false;
      return boosterStatus(was ? "Undone. Priority, GPU choice and the power plan are back to normal." : "Nothing to undo.");
    },
    async ask_ai(a) {
      await sleep(300);
      const base = a.service === "chatgpt" ? "https://chatgpt.com/?q=" : "https://claude.ai/new?q=";
      const lines = [`I need help with Guarana, a Switch 2 emulator for Windows. First read its docs at https://karimalsalah.github.io/guarana/llms-full.txt . My setup:`, `GPU Intel Arc 140T.`, ...((a.issues as string[] | undefined) ?? []).map((i) => `Doctor: ${i}`)];
      const prompt = lines.join(" | ");
      const url = base + encodeURIComponent(prompt);
      console.info("[mock] ask_ai", url);
      return { service: a.service === "chatgpt" ? "chatgpt" : "claude", url, prompt, url_chars: url.length } satisfies AskAiResult;
    },
    async launch_game(a) {
      await sleep(900);
      const id = String(a.title_id ?? "");
      if (warming.has(id)) throw `Shaders for this game are still being prepared (${warming.get(id)}%). It can start when that finishes.`;
      if (gameRunning) throw "Guarana is already running.";
      if (sc.fail && firstLaunch) {
        firstLaunch = false;
        throw "guarana.exe stopped during startup (0xC0000135): a DLL is missing. Doctor can check the Visual C++ runtime.";
      }
      if (!a.title_id && !a.path) throw "Nothing to launch.";
      gameRunning = true;
      if (boosterLevel >= 1) {
        boosterActive = true;
        emit("booster://changed", boosterStatus());
      }
      // game://boot as boot.rs emits it: the session output of a 20.3+ emulator (boot=session), only
      // its window taking the foreground (boot=old, an older version), or nothing (boot=none).
      const boot = (at: number, phase: BootProgress["phase"], done = 0, total = 0) =>
        setTimeout(() => {
          if (gameRunning) emit("game://boot", { phase, done, total } satisfies BootProgress);
        }, at * sc.speed);
      if (sc.boot === "session") {
        boot(100, "starting");
        boot(600, "loading");
        boot(1400, "window");
        boot(1500, "shaders");
        for (let i = 0; i <= 10; i++) boot(1700 + i * 200, "shaders", Math.round((1346 * i) / 10), 1346);
        boot(3900, "launching");
        boot(4600, "first-frame");
        boot(5100, "shown");
      } else if (sc.boot === "old") {
        boot(1200, "window");
      }
      const exited: GameExited = sc.crash ? { code: 3221225477, seconds: 1 } : { code: 0, seconds: 2715 };
      setTimeout(() => {
        gameRunning = false;
        if (boosterActive) {
          boosterActive = false;
          emit("booster://changed", boosterStatus());
        }
        emit("game://exited", exited);
        // A clean exit with a minute of gameplay data: the backend offers this session's numbers.
        if (exited.code === 0 && sharePrompt && String(a.title_id ?? "") === OOT_ID) {
          sessionIndex += 1;
          offerAt = new Date().toISOString();
          offerSecs = Math.max(60, Math.round(exited.seconds * 0.96));
          shareOn = true;
          emit("share://changed", shareStatus());
        }
        // The session runs once the game is on screen (after the boot above).
      }, sc.crash ? 1400 : sc.session * 1000 + (sc.boot === "session" ? 5100 : sc.boot === "old" ? 1200 : 0) * sc.speed);
      return { pid: 23816, follows_boot: sc.boot === "session" };
    },
    async collect_report(a) {
      await sleep(1400);
      if (sc.fail) throw "Couldn't write the report zip: the disk is full.";
      const kind = (a.kind as ReportKind) ?? "bug";
      const template = kind === "crash" ? "crash.yml" : kind === "performance" ? "device-report.yml" : "game_bug.yml";
      const note = typeof a.note === "string" && a.note.trim() ? a.note.trim() : "";
      const label = kind === "crash" ? "Crash" : kind === "performance" ? "Performance" : "Bug";
      const summary = [
        `**Kind:** ${label}`,
        `**Guarana:** v${state.current ?? "?"} (stable) · Launcher 0.1.0`,
        `**Device:** ${SYSTEM.device.manufacturer} ${SYSTEM.device.model}, ${SYSTEM.device.form}`,
        `**GPU:** Intel Arc 140T, driver \`${SYSTEM.gpus[0].driver_version}\``,
        `**CPU:** ${SYSTEM.cpu.name} · ${SYSTEM.ram_gb} GB RAM`,
        `**Display:** ${SYSTEM.display.width} × ${SYSTEM.display.height} at ${SYSTEM.display.refresh_hz} Hz`,
        `**Profile:** ${state.profile_id ?? "none"}`,
        note ? `**Your note:** ${note}` : "",
        "",
        "In the zip: `eden_log.txt`, `qt-config.ini`, `nx2_switches.txt`, `doctor.json`, `system.json`.",
        "Paths show as `%USERPROFILE%` and the host name is removed.",
      ].filter((l, i, all) => l !== "" || all[i - 1] !== "").join("\n");
      const r: Report = {
        zip_path: `${HOME}\\AppData\\Local\\Guarana\\reports\\guarana-report-20261113-1412.zip`,
        issue_url: `https://github.com/karimalsalah/guarana/issues/new?template=${template}&title=${encodeURIComponent(`[${label}] `)}`,
        summary_md: summary,
      };
      return r;
    },
    async open_path(a) {
      await sleep(120);
      console.info("[mock] open_path", a.path);
      return null;
    },
    async open_url(a) {
      await sleep(120);
      console.info("[mock] open_url", a.url);
      return null;
    },
    async list_controllers() {
      await sleep(180);
      return controllers;
    },
  };

  return {
    async invoke<R>(cmd: string, args?: Record<string, unknown>): Promise<R> {
      const fn = commands[cmd];
      if (!fn) throw `command ${cmd} not found`;
      const result = await fn(args ?? {});
      return clone(result) as R;
    },
    async listen<P>(name: string, handler: (payload: P) => void): Promise<Unlisten> {
      const set = listeners.get(name) ?? new Set<(payload: unknown) => void>();
      const h = (payload: unknown) => handler(payload as P);
      set.add(h);
      listeners.set(name, set);
      return () => set.delete(h);
    },
    async appVersion() {
      return "0.1.0";
    },
  };
}
