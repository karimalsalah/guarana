// Home, Ocarina-first: a hero card for Ocarina of Time with one big Play, the mode, the seed with the
// last session's numbers under it, the rest of the library (lazy), then "This PC" (Booster,
// controllers, setup). Warms the shader cache once, in the background.

import { api, errorText, events } from "../api";
import { h } from "../dom";
import { duration, exitCode, relativeTime, v } from "../format";
import { rumble } from "../gamepad";
import { icon } from "../icons";
import { settleFocus } from "../input";
import { reducedMotion, whenIdle } from "../motion";
import { go, type ScreenContext, type ScreenDef } from "../router";
import { lastPacing, lastSession, recordPacing } from "../sessions";
import { launcher, library, profiles, Signal, warmStatus } from "../state";
import type { BoosterStatus, Game, LauncherState, ModeId, ModeStatus, Profile, ShareStatus, UpdateInfo, WarmResult } from "../types";
import { badge, busy, button, card, segmented, skeleton, spinner, switchControl } from "../ui/controls";
import { controllersCard } from "../ui/controllers-card";
import { fixed1, odometer, pacingStrip, readout, type Odometer, type Pacing } from "../ui/numbers";
import { seed, type Seed } from "../ui/seed";
import { toast, type Tone } from "../ui/toast";

// The last finished session's numbers. Their only source is the backend's "Share my numbers" data
// (get_share_status and share://changed: the emulator's log of a finished session). The latest per
// game is kept (sessions.ts), because the backend drops the offer after Share or Not now.
/** The latest offer seen, keyed by title id (or game name when it has none). */
const pacingSeen = new Signal<{ key: string; p: Pacing } | null>(null);
/** Whether the backend asks to share at all (false after "Don't ask again": then no new numbers come). */
const shareAsks = new Signal<boolean | null>(null);

// Prefetch (prefetch_caches: shader cache files into the OS file cache) once per session, per title.
const warm = { titleId: null as string | null, state: "idle" as "idle" | "warming" | "done" | "failed", bytes: 0 };
const warmSubs = new Set<() => void>();
const setWarm = (s: typeof warm.state, bytes = 0) => {
  warm.state = s;
  warm.bytes = bytes;
  for (const fn of warmSubs) fn();
};

// One update check per session, in the background, for the badge on the setup card.
let updateCheck: Promise<UpdateInfo | null> | null = null;

/** Forgets the update check and clears the Updates tab's dot. Updates calls it after an install or a switch. */
export function resetUpdateCheck(): void {
  updateCheck = null;
  document.querySelector('.tab[data-tab="updates"]')?.classList.remove("has-dot");
}

// Chips: the profile's switches that change how the game looks or paces, when they are on.
const HIGHLIGHTS = [
  { id: "fluid_presenter", label: "Even frame pacing" },
  { id: "frame_generation", label: "Frame generation" },
  { id: "hdr_auto", label: "HDR when available" },
  { id: "async_shaders", label: "Async shaders" },
];

/** What a finished warm_shaders run means, in one line. */
function warmResultText(r: WarmResult): { text: string; tone: Tone } {
  switch (r.status) {
    case "done":
      return { text: `Shaders ready: ${r.built.toLocaleString()} built in ${duration(r.seconds)}.`, tone: "ok" };
    case "nothing to do":
      // warm.rs says why: this build can't prepare shaders ahead, or the game has no cache yet.
      return { text: r.message || "Shaders were already prepared.", tone: r.message ? "info" : "ok" };
    case "nothing built":
      return { text: "Nothing could be prepared ahead for this game. It builds shaders as you play.", tone: "info" };
    case "incomplete":
    case "stopped":
      return { text: `Stopped with ${r.left.toLocaleString()} left. Prepare again to finish.`, tone: "warn" };
    default:
      return { text: r.message ?? "Preparing shaders didn't work.", tone: "fail" };
  }
}

function toggleOn(p: Profile | undefined, id: string): boolean {
  return p?.toggles.find((t) => t.id === id)?.value === true;
}

const screen: ScreenDef = {
  title: "Home",
  chrome: "full",
  tab: "home",

  render(ctx) {
    const exited = ctx.params.exited as { code: number; seconds: number; title: string } | undefined;
    // The seed: the heart of Home. Half-awake while the library loads; it opens when the game is
    // ready (back from a game, it wakes up), stays closed until the keys are in, and shuts on an error.
    const theSeed = seed({ size: "stage", state: exited ? "closed" : "drowsy", follow: true, breathe: true, echoes: true });
    const heroSlot = h("div", { class: "hero-slot" }, heroPlaceholder());
    const setupSlot = h("div", { class: "setup-slot" }, card("Your setup", { icon: "shield" }, skeleton([80, 60, 70])));
    const libTitle = h("h2", { class: "section-title", id: "lib-title", text: "Library" });
    const libBody = h("div", { class: "library-body" }, skeleton([55, 40, 48], "skeleton-tall"));
    const libSection = h("section", { class: "home-act library", "aria-labelledby": "lib-title" }, h("div", { class: "section-head" }, libTitle), libBody);

    const modeSlot = h("div", { class: "mode-slot" });
    const boosterSlot = h("div", { class: "booster-slot" });
    const shareSlot = shareCard(ctx);
    // The last session's numbers sit under the seed: what the eye saw. Filled once the game is ready.
    const numbersSlot = h("div", { class: "numbers-slot" });
    // The game is the stage: the seed, the title and Play, and how it runs. Everything else is
    // below it, quieter: the rest of the library, then the PC's own panes (Booster, controllers, setup).
    const stage = h(
      "div",
      { class: "home-stage" },
      h("div", { class: "stage-eye" }, h("div", { class: "stage-seed" }, theSeed.el), numbersSlot),
      h("div", { class: "stage-copy" }, heroSlot, modeSlot),
      shareSlot,
      h("p", { class: "stage-cue", "aria-hidden": "true" }, h("span", { text: "Library and this PC" }), icon("chevronDown")),
    );
    const pcSection = h(
      "section",
      { class: "home-act home-pc", "aria-labelledby": "pc-title" },
      h("div", { class: "section-head" }, h("h2", { class: "section-title", id: "pc-title", text: "This PC" })),
      boosterSlot,
      h("div", { class: "home-grid" }, controllersCard(ctx.signal), setupSlot),
    );
    const el = h("div", { class: "home" }, stage, libSection, pcSection);

    if (exited) {
      setTimeout(() => {
        if (exited.code === 0) toast(`Welcome back. You played ${duration(exited.seconds)} of ${exited.title}.`, { tone: "ok" });
        else
          toast(`${exited.title} closed unexpectedly (exit code ${exitCode(exited.code)}).`, {
            tone: "fail",
            action: { label: "Report it", run: () => void go("report", { params: { kind: "crash" } }) },
          });
      }, 500);
    }

    void (async () => {
      const loaded = await Promise.all([launcher.get(true), library.get(true)]).catch((e: unknown) => {
        if (!ctx.signal.aborted) {
          heroSlot.replaceChildren(errorHero(errorText(e)));
          theSeed.setState("shut");
        }
        return null;
      });
      if (!loaded) return;
      const [state, games] = loaded;
      const profs = await profiles.get().catch(() => [] as Profile[]);
      if (ctx.signal.aborted) return;

      // list_profiles already merges the user's saved values into the applied profile.
      const profileId = state.profile_id;
      const profile = profs.find((p) => p.id === profileId);
      const oot = games.find((g) => g.is_ocarina);

      heroSlot.replaceChildren(renderHero(ctx, theSeed, state, oot, profile, profileId, !!exited, numbersSlot));
      if (state.keys_present || state.profile_id) {
        modeSlot.replaceChildren(modeCard(ctx));
        boosterSlot.replaceChildren(boosterCard(ctx));
      }
      setupSlot.replaceChildren(setupCard(state, profile, ctx));
      settleFocus(el);

      const rest = oot && state.keys_present ? games.filter((g) => g !== oot) : games;
      libTitle.textContent = oot && state.keys_present ? "More games" : "Library";
      renderLibrary(libSection, libBody, rest, state, profileId, ctx, theSeed);
    })();

    return el;
  },
};

export default screen;

/** Where the interface folds to on Play: the seed's eye if it is on screen, else `fallback`. */
function foldInto(theSeed: Seed, fallback?: HTMLElement): boolean {
  const root = document.documentElement;
  const main = document.getElementById("main")?.getBoundingClientRect();
  const r = theSeed.el.getBoundingClientRect();
  const visible = r.bottom > 40 && r.top < innerHeight - 40 && r.width > 0;
  const at = visible ? { x: r.left + r.width * 0.56, y: r.top + r.height * 0.5 } : (() => {
    const f = (fallback ?? theSeed.el).getBoundingClientRect();
    return { x: f.left + Math.min(f.width / 2, 120), y: f.top + f.height / 2 };
  })();
  root.style.setProperty("--fold-x", `${Math.round(at.x)}px`);
  root.style.setProperty("--fold-y", `${Math.round(at.y)}px`);
  root.style.setProperty("--fold-ox", `${Math.round(main?.left ?? 0)}px`);
  root.style.setProperty("--fold-oy", `${Math.round(main?.top ?? 0)}px`);
  if (visible) {
    theSeed.el.style.setProperty("view-transition-name", "seed");
    theSeed.dilate(1.3);
  }
  return visible;
}

function heroPlaceholder(): HTMLElement {
  return h(
    "article",
    { class: "hero is-loading", "aria-busy": "true", "aria-label": "Loading your library" },
    h("div", { class: "hero-body" }, h("div", { class: "skeleton hero-skeleton" }, h("span", { class: "skeleton-line" }), h("span", { class: "skeleton-line" }), h("span", { class: "skeleton-line" }))),
  );
}

function errorHero(message: string): HTMLElement {
  return h(
    "article",
    { class: "hero hero-error" },
    h(
      "div",
      { class: "hero-body" },
      h("p", { class: "eyebrow" }, icon("fail"), "Something's off"),
      h("h1", { class: "hero-title hero-title-sm", text: "Your library didn't load" }),
      h("p", { class: "hero-meta", text: message }),
      h("div", { class: "hero-actions" }, button("Try again", { kind: "primary", size: "lg", icon: "refresh", primary: true, autofocus: true, onClick: () => void go("home", { replace: true, transition: "fade" }) })),
    ),
  );
}

/** What a filled numbers slot needs undone before it is filled again or emptied: its subscriptions. */
const numbersCleanup = new WeakMap<HTMLElement, () => void>();

/** Empties the numbers slot: the pane goes, its subscriptions end and the seed loses its ring. */
function clearNumbers(numbersSlot: HTMLElement, theSeed: Seed): void {
  numbersCleanup.get(numbersSlot)?.();
  numbersCleanup.delete(numbersSlot);
  numbersSlot.replaceChildren();
  theSeed.session(null);
}

function renderHero(ctx: ScreenContext, theSeed: Seed, state: LauncherState, oot: Game | undefined, profile: Profile | undefined, profileId: string | null, waking: boolean, numbersSlot: HTMLElement): HTMLElement {
  // Every branch starts from an empty slot: numbers from an earlier pass never outlive it.
  clearNumbers(numbersSlot, theSeed);
  if (!state.keys_present) {
    // The fruit stays closed until the keys are in.
    theSeed.setState("closed");
    return h(
      "article",
      { class: "hero hero-pending", "aria-labelledby": "hero-title" },
      h(
        "div",
        { class: "hero-body" },
        h("p", { class: "eyebrow", text: "Almost ready" }),
        h("h1", { class: "hero-title", id: "hero-title", text: oot ? oot.title : "Your games" }),
        h("p", { class: "hero-meta", text: "Guarana needs your keys before a game can start. Dump prod.keys from your console, then import it." }),
        h("div", { class: "hero-actions" }, button("Find my keys", { kind: "primary", size: "xl", icon: "key", primary: true, autofocus: true, onClick: () => void go("scan") })),
      ),
    );
  }

  if (!oot) {
    theSeed.setState("drowsy");
    return h(
      "article",
      { class: "hero hero-pending", "aria-labelledby": "hero-title" },
      h(
        "div",
        { class: "hero-body" },
        h("p", { class: "eyebrow", text: "Ocarina first" }),
        h("h1", { class: "hero-title", id: "hero-title", text: "Add Ocarina of Time" }),
        h("p", { class: "hero-meta", text: "Guarana is tuned around it first: its own profile, motion data and measured fixes. Other games run too." }),
        h("div", { class: "hero-actions" }, button("Find my games", { kind: "primary", size: "xl", icon: "search", primary: true, autofocus: true, onClick: () => void go("scan", { params: { deep: true } }) })),
      ),
    );
  }

  // Ready: the fruit splits open. Back from a game it was closed, so this reads as waking up.
  if (waking) setTimeout(() => !ctx.signal.aborted && theSeed.setState("open"), 380);
  else theSeed.setState("open");

  const title = h("h1", { class: "hero-title", id: "hero-title", text: oot.title });
  const session = lastSession(oot.title_id);
  const meta = session ? `Last played ${relativeTime(session.last)} · ${duration(session.seconds)}` : "Ready for your first session";
  const metaEl = h("p", { class: "hero-meta" }, icon("clock"), meta);
  // The measured session's numbers under the seed. "Last played" stays here: it is the launcher's
  // own record and can be newer than the measured session (a play under a minute is not measured).
  // Without a launcher record (the game was last played outside the launcher, or its record was
  // cleared), "Ready for your first session" would contradict measured numbers: it steps aside then.
  const numbers = sessionNumbers(ctx, theSeed, oot, (has) => {
    if (!session) metaEl.hidden = has;
  });
  numbersCleanup.set(numbersSlot, numbers.dispose);
  numbersSlot.replaceChildren(numbers.el);
  const chips = h(
    "ul",
    { class: "chips", "aria-label": "Profile" },
    h("li", { class: "chip" }, icon("sliders"), profile ? profile.name : "No profile yet"),
    HIGHLIGHTS.filter((x) => toggleOn(profile, x.id))
      .slice(0, 3)
      .map((x) => h("li", { class: "chip" }, icon("leaf"), x.label)),
  );
  const warmEl = h("span", { class: "warm", role: "status" });
  const play = button("Play", {
    kind: "play",
    size: "xl",
    icon: "play",
    primary: true,
    autofocus: true,
    attrs: { "aria-describedby": "hero-title" },
    onClick: () => {
      rumble(60);
      // The interface folds into the seed, and the seed carries the launch.
      const morphSeed = foldInto(theSeed);
      title.style.setProperty("view-transition-name", "hero-title");
      void go("launch", { params: { game: oot, profileId, morph: morphSeed ? true : "title" }, transition: "launch" }).catch((e: unknown) => {
        theSeed.el.style.removeProperty("view-transition-name");
        theSeed.dilate(1);
        title.style.removeProperty("view-transition-name");
        toast(errorText(e), { tone: "fail" });
      });
    },
  });
  // The seed notices Play: its echo arcs (predicted frames) slide out and the pupil widens.
  const interested = (on: boolean) => {
    theSeed.ready(on && !play.disabled);
    theSeed.dilate(on && !play.disabled ? 1.16 : 1);
  };
  play.addEventListener("focus", () => interested(true));
  play.addEventListener("blur", () => interested(false));
  play.addEventListener("pointerenter", () => interested(true));
  play.addEventListener("pointerleave", () => interested(document.activeElement === play));

  // "Prepare shaders" (warm_shaders). While this title's shaders build, from here or the automatic
  // run after a driver change, Play waits: the backend refuses to start the same title meanwhile.
  const prepare = button("Prepare shaders", {
    kind: "ghost",
    size: "lg",
    icon: "sparkle",
    attrs: { title: "Builds this game's shaders now, so first visits hitch less" },
    onClick: (_e, self) =>
      void busy(self, () => api.warmShaders(oot.title_id), "Starting…")
        .then((r) => {
          const t = warmResultText(r);
          toast(t.text, { tone: t.tone });
        })
        .catch((e: unknown) => toast(errorText(e), { tone: "fail" })),
  });
  const warmFill = h("span", { class: "warm-bar-fill" });
  const warmLine = h("p", { class: "warm-line", id: "warm-line", role: "status" });
  const stop = h("button", { type: "button", class: "link-btn" }, "Stop");
  stop.addEventListener("click", () => {
    stop.disabled = true;
    void api
      .stopWarmShaders()
      .then((r) => toast(r.message, { tone: r.stopped ? "info" : "warn" }))
      .catch((e: unknown) => toast(errorText(e), { tone: "fail" }))
      .finally(() => {
        stop.disabled = false;
      });
  });
  const warmBox = h("div", { class: "warm-box", hidden: true }, h("div", { class: "warm-bar", "aria-hidden": "true" }, warmFill), h("div", { class: "row" }, warmLine, stop));
  const renderShaderWarm = () => {
    const w = warmStatus.value[oot.title_id];
    const active = !!w && w.stage !== "done";
    play.disabled = active;
    prepare.hidden = active;
    warmBox.hidden = !active;
    warmEl.hidden = active;
    if (active) play.setAttribute("aria-describedby", "warm-line");
    else play.setAttribute("aria-describedby", "hero-title");
    if (!w || !active) return;
    warmFill.style.transform = `scaleX(${Math.max(0.02, Math.min(1, w.percent / 100))})`;
    const what = w.auto ? "Rebuilding shaders after your driver update" : "Preparing shaders";
    warmLine.textContent = `${what} · ${Math.round(w.percent)}%${w.pass > 1 ? ` · pass ${w.pass}` : ""}. Play unlocks when it's done.`;
  };
  warmStatus.subscribe(renderShaderWarm, ctx.signal);

  const hero = h(
    "article",
    { class: "hero hero-ready", "aria-labelledby": "hero-title" },
    h(
      "div",
      { class: "hero-body" },
      h("p", { class: "eyebrow", text: "Ready to play" }),
      title,
      metaEl,
      chips,
      h("div", { class: "hero-actions" }, play, prepare, warmEl),
      warmBox,
      profileId === "intel-arc-igpu"
        ? h("p", { class: "hero-fact" }, icon("gauge"), "Measured on an Arc 140T: Hyrule Field 14.75 → 22.83 game fps, frame generation off.")
        : null,
    ),
  );
  renderShaderWarm();

  // Warm the shader and pipeline caches once, when the main thread is idle.
  const renderWarm = () => {
    if (!warmEl.isConnected && warm.state !== "warming") return;
    if (warm.state === "warming") warmEl.replaceChildren(spinner("warm-spin"), "Warming up shaders…");
    else if (warm.state === "done" && warm.bytes > 0) {
      // The cache files are in memory now; building missing shaders is "Prepare shaders".
      warmEl.replaceChildren(icon("ok"), "Shader cache loaded");
      warmEl.classList.add("is-done");
      setTimeout(() => warmEl.classList.add("is-faded"), 4000);
    } else warmEl.replaceChildren();
  };
  warmSubs.add(renderWarm);
  ctx.onLeave(() => warmSubs.delete(renderWarm));
  if (warm.titleId !== oot.title_id || warm.state === "idle") {
    warm.titleId = oot.title_id;
    const cancel = whenIdle(() => {
      setWarm("warming");
      api.prefetchCaches(oot.title_id).then(
        (r) => setWarm("done", r.bytes_warmed),
        () => setWarm("failed"),
      );
    }, 1500);
    ctx.onLeave(() => {
      if (warm.state === "idle") {
        cancel();
        warm.titleId = null;
      }
    });
  } else {
    renderWarm();
    if (warm.state === "done") warmEl.classList.add("is-faded");
  }
  return hero;
}

// ---- The session's numbers --------------------------------------------------------------------------

/**
 * The last measured session of `game`, under the seed: frames on time as the large numeral, the
 * pacing strip, and game fps, frame time, shown fps and when it was measured (the session's own date,
 * never the launcher's last-play time, which can be newer). The seed wears the frames-on-time share
 * as its ring. With no finished session, a line says when the numbers appear.
 * `onShown(has)` says whether numbers are on screen. `dispose` ends its subscriptions (also done when
 * the screen is left).
 */
function sessionNumbers(ctx: ScreenContext, theSeed: Seed, game: Game, onShown: (has: boolean) => void): { el: HTMLElement; dispose: () => void } {
  const keys = [game.title_id, game.title].filter(Boolean);
  const stored = (): Pacing | null => {
    const seen = pacingSeen.value;
    if (seen && keys.includes(seen.key)) return seen.p;
    for (const k of keys) {
      const p = lastPacing(k);
      if (p) return p;
    }
    return null;
  };

  const el = h("section", { class: "numbers", "aria-labelledby": "numbers-title" });
  let parts: {
    onTime: Odometer;
    fps: Odometer;
    p50: Odometer;
    shown: Odometer;
    p95Note: HTMLElement;
    measured: HTMLElement;
    measuredNote: HTMLElement;
    strip: ReturnType<typeof pacingStrip>;
  } | null = null;
  let shownAt = "";
  let alive = true;

  const emptyLine = (): string => {
    if (shareAsks.value === false)
      return "Session numbers come with “Ask to share my numbers”, which is off. Turn it back on under Advanced to see them here after your next session.";
    if (lastSession(game.title_id))
      return "They appear here after a session with at least a minute of play: frames on time, game fps and the pace of shown frames, from the emulator's own log.";
    return "They appear here after your first session: how many frames arrive on time, game fps and the pace of shown frames, from the emulator's own log.";
  };

  const showEmpty = () => {
    parts = null;
    shownAt = "";
    el.className = "numbers is-empty";
    el.replaceChildren(h("p", { class: "numbers-kicker", id: "numbers-title", text: "Your numbers" }), h("p", { class: "numbers-empty", text: emptyLine() }));
    theSeed.session(null);
    onShown(false);
  };

  const build = () => {
    const onTime = odometer("00.0", { cls: "numbers-big-odo", label: "percent of frames on time" });
    const fps = odometer("00.0", { label: "game frames per second" });
    const p50 = odometer("00.0", { label: "milliseconds, median time between shown frames" });
    const shown = odometer("00.0", { label: "frames per second shown" });
    const p95Note = h("span", { class: "readout-note" });
    const measured = h("span", { class: "readout-text" });
    const measuredNote = h("span", { class: "readout-note" });
    const strip = pacingStrip({ seed: game.title_id || game.title, cls: "numbers-strip" });
    el.className = "numbers";
    el.replaceChildren(
      h("p", { class: "numbers-kicker", id: "numbers-title", text: "Your last measured session" }),
      h(
        "div",
        { class: "numbers-hero" },
        h("p", { class: "numbers-big" }, onTime.el, h("span", { class: "numbers-pct", "aria-hidden": "true", text: "%" })),
        h("p", { class: "numbers-big-label" }, h("strong", { text: "Frames on time" }), h("span", { text: "shown frames within 2 ms of the median" })),
      ),
      strip.el,
      h(
        "div",
        { class: "numbers-readouts" },
        readout("Game fps", fps.el),
        readout("Between frames", p50.el, { unit: "ms", note: p95Note }),
        readout("Shown fps", shown.el),
        readout("Measured", measured, { note: measuredNote, cls: "readout-played" }),
      ),
    );
    parts = { onTime, fps, p50, shown, p95Note, measured, measuredNote, strip };
    return parts;
  };

  const show = (p: Pacing | null) => {
    if (ctx.signal.aborted || !alive) return;
    if (!p) {
      if (parts) return; // keep numbers already shown; the offer going away doesn't unmeasure them
      showEmpty();
      return;
    }
    if (p.at === shownAt) return;
    const fresh = !parts;
    const x = parts ?? build();
    shownAt = p.at;
    x.strip.set(p);
    x.p95Note.textContent = `shown frames, median; 95% < ${fixed1(p.p95)}`;
    // When the measured session ended and how much play it measured: both from the offer itself.
    x.measured.textContent = relativeTime(p.at) || "–";
    x.measuredNote.textContent = `${duration(p.gameplay_secs)} of play`;
    const roll = () => {
      x.onTime.set(fixed1(p.within2));
      x.fps.set(fixed1(p.game_fps));
      x.p50.set(fixed1(p.p50));
      x.shown.set(fixed1(p.shown_fps));
    };
    if (fresh && !reducedMotion()) {
      // Arrive at zero, then roll up and let the stems grow once the stage is in place. The timer
      // makes sure the real figures land even where frames are not drawn (a hidden window).
      let done = false;
      const once = () => {
        if (done || shownAt !== p.at) return;
        done = true;
        roll();
      };
      requestAnimationFrame(() => requestAnimationFrame(once));
      setTimeout(once, 250);
      x.strip.reveal(160);
    } else roll();
    theSeed.session(p.within2 / 100);
    onShown(true);
  };

  show(stored());
  const offSeen = pacingSeen.subscribe((seen) => {
    if (seen && keys.includes(seen.key)) show(seen.p);
  }, ctx.signal);
  const offAsks = shareAsks.subscribe(() => {
    if (!parts) showEmpty();
  }, ctx.signal);
  const dispose = () => {
    alive = false;
    offSeen();
    offAsks();
  };
  return { el, dispose };
}

function setupCard(state: LauncherState, profile: Profile | undefined, ctx: ScreenContext): HTMLElement {
  const versionRow = h("li", { class: "setup-row" }, icon("download"), h("span", { class: "grow" }, "Guarana ", h("strong", { text: v(state.current) })));
  const rows = h(
    "ul",
    { class: "setup-list" },
    h("li", { class: "setup-row" }, icon("key"), h("span", { class: "grow", text: "Keys" }), state.keys_present ? badge("Imported", "ok", "ok") : badge("Missing", "fail", "fail")),
    h("li", { class: "setup-row" }, icon("chip"), h("span", { class: "grow", text: "Firmware" }), state.firmware_present ? badge("Installed", "ok", "ok") : badge("Missing", "warn", "warn")),
    h(
      "li",
      { class: "setup-row" },
      icon("sliders"),
      h("span", { class: "grow" }, "Profile ", h("strong", { text: profile ? profile.name : "not set" })),
      h("button", { type: "button", class: "link-btn", onClick: () => void go("profile") }, "Change"),
    ),
    versionRow,
  );

  if (!updateCheck) updateCheck = api.checkUpdates().catch(() => null);
  void updateCheck.then((info) => {
    // The check is once per session: after an install or a switch, `latest` may be the version in use.
    if (ctx.signal.aborted || !info?.update_available || info.latest === state.current) return;
    versionRow.appendChild(h("button", { type: "button", class: "link-btn", onClick: () => void go("updates") }, `${v(info.latest)} is out`, icon("chevronRight")));
    document.querySelector('.tab[data-tab="updates"]')?.classList.add("has-dot");
  });

  return card("Your setup", { icon: "shield" }, rows);
}

function renderLibrary(section: HTMLElement, body: HTMLElement, games: Game[], state: LauncherState, profileId: string | null, ctx: ScreenContext, theSeed: Seed): void {
  if (!games.length) {
    body.replaceChildren(
      h(
        "div",
        { class: "empty" },
        h("span", { class: "empty-icon", "aria-hidden": "true" }, icon("game")),
        h("p", { class: "empty-title", text: state.game_folders.length ? "No other games yet" : "No games yet" }),
        h("p", { class: "fine", text: "Guarana looks in the folders you add. It never downloads games." }),
        button("Find my games", { kind: "secondary", icon: "search", onClick: () => void go("scan", { params: { deep: true } }) }),
      ),
    );
    return;
  }
  const load = () =>
    import("../ui/library").then((m) => {
      if (ctx.signal.aborted) return;
      body.replaceChildren(
        m.libraryList(games, {
          canPlay: state.keys_present,
          // The row's title carries into the launch stage, like the hero's does.
          onPlay: (g, titleEl) => {
            rumble(60);
            // Folds into the seed when it is on screen, else into the row itself.
            const morphSeed = foldInto(theSeed, titleEl);
            titleEl.style.setProperty("view-transition-name", "hero-title");
            void go("launch", { params: { game: g, profileId, morph: morphSeed ? true : "title" }, transition: "launch" }).catch((e: unknown) => {
              theSeed.el.style.removeProperty("view-transition-name");
              theSeed.dilate(1);
              titleEl.style.removeProperty("view-transition-name");
              toast(errorText(e), { tone: "fail" });
            });
          },
        }),
      );
    });
  if (typeof IntersectionObserver !== "function") {
    void load();
    return;
  }
  const io = new IntersectionObserver(
    (entries) => {
      if (entries.some((e) => e.isIntersecting)) {
        io.disconnect();
        void load();
      }
    },
    { root: document.getElementById("main"), rootMargin: "240px" },
  );
  io.observe(section);
  ctx.onLeave(() => io.disconnect());
}

// ---- Share my numbers -----------------------------------------------------------------------------------

/** Opens the prefilled device-report form. Nothing is sent: the user reviews and submits on GitHub. */
async function shareNow(): Promise<void> {
  const link = await api.shareOpen();
  if (link.level !== "full") {
    // The link could not carry everything: the whole text goes to the clipboard to paste into the form.
    try {
      await navigator.clipboard.writeText(link.text);
      toast(link.level === "short" ? "The link had room for the essentials only. The full text is on your clipboard." : "Too long for a link. The form opens empty and the text is on your clipboard: paste it in.", { tone: "info" });
    } catch {
      toast("Couldn't copy the text. Use Report a problem for a zip with the details.", { tone: "warn" });
    }
  }
  try {
    await api.openUrl(link.url);
  } catch (e) {
    // The browser would not open: the text is the fallback, and the card stays for another try.
    try {
      await navigator.clipboard.writeText(link.text);
      toast(`Couldn't open the browser (${errorText(e)}). The text is on your clipboard: paste it into a new device report on GitHub.`, { tone: "warn" });
    } catch {
      toast(`Couldn't open the browser: ${errorText(e)}`, { tone: "fail" });
    }
    return;
  }
  await api.shareDismiss();
}

function shareCard(ctx: ScreenContext): HTMLElement {
  const text = h("p", { class: "share-text", role: "status" });
  const fine = h("p", { class: "fine", text: "Nothing is sent. Your browser opens GitHub's device report with these numbers filled in. You add the scene and a screenshot, read it, and press Submit yourself." });
  // Secondary on purpose: Play is the one ember button on Home.
  const share = button("Share", { kind: "secondary", icon: "sparkle", onClick: (_e, self) => void busy(self, shareNow, "Opening…").catch((e: unknown) => toast(errorText(e), { tone: "fail" })) });
  const later = button("Not now", { kind: "ghost", onClick: () => void api.shareDismiss().then(show).catch(() => undefined) });
  const never = h("button", { type: "button", class: "link-btn" }, "Don't ask again");
  never.addEventListener("click", () => {
    void api
      .shareSetPrompt(false)
      .then((st) => {
        show(st);
        toast("Okay. You can turn this back on under Advanced.", { tone: "info" });
      })
      .catch(() => undefined);
  });
  const el = h("section", { class: "card share-card", hidden: true, "aria-label": "Share my numbers" }, text, fine, h("div", { class: "row" }, share, later, never));
  function show(st: ShareStatus): void {
    if (ctx.signal.aborted) return;
    el.hidden = !st.offer;
    text.textContent = st.offer?.text ?? "";
    const o = st.offer;
    if (o) {
      // The offer is the finished session's numbers: kept for the stage, also after Share or Not now.
      const key = o.title_id || o.game;
      const p: Pacing = { at: o.at, game_fps: o.game_fps, within2: o.within2, shown_fps: o.shown_fps, p50: o.p50, p95: o.p95, p99: o.p99, gameplay_secs: o.gameplay_secs };
      recordPacing(key, p);
      const seen = pacingSeen.value;
      if (!seen || seen.key !== key || new Date(seen.p.at).getTime() <= new Date(p.at).getTime()) pacingSeen.set({ key, p });
    }
    if (shareAsks.value !== st.enabled) shareAsks.set(st.enabled);
  }
  void api.getShareStatus().then(show, () => undefined);
  void events.shareChanged(show, ctx.signal).catch(() => undefined);
  return el;
}

// ---- The one switch: Auto, Smooth, Quality, Battery ---------------------------------------------------

const MODES: { value: ModeId; label: string; hint: string }[] = [
  { value: "auto", label: "Auto", hint: "Picks everything from your PC and what other players found" },
  { value: "smooth", label: "Smooth", hint: "Steadier frames, a little less resolution" },
  { value: "quality", label: "Quality", hint: "A sharper picture, more GPU work" },
  { value: "battery", label: "Battery", hint: "Lower power: 0.75x and a 30 fps cap" },
];

function communityLine(st: ModeStatus): string {
  const c = st.community;
  const src = c.source === "network" ? "fetched today" : c.source === "cache" ? "saved on this PC" : "built in, offline";
  if (!c.label) return "";
  if (c.samples === 0) return `No community reports for ${c.label} yet (${src}).`;
  return `Community: ${c.validated} of ${c.samples} reports for ${c.label} are verified${c.validated >= c.min_validated ? "" : `; ${c.min_validated} are needed before Auto uses them`} (${src}).`;
}

function modeCard(ctx: ScreenContext): HTMLElement {
  const reason = h("p", { class: "mode-reason", role: "status" });
  const note = h("p", { class: "fine mode-note", hidden: true });
  const auto = h("p", { class: "autopilot-line", role: "status" });
  const undo = button("Undo", { kind: "ghost", size: "sm", icon: "history" });
  const autoRow = h("div", { class: "autopilot-row", hidden: true }, icon("sparkle"), auto, undo);
  const seg = segmented({
    choices: MODES,
    value: "auto",
    label: "Mode",
    onChange: (value) => {
      void api
        .setMode(value as ModeId)
        .then((st) => {
          show(st);
          void profiles.get(true);
          void launcher.get(true);
        })
        .catch((e: unknown) => {
          toast(errorText(e), { tone: "fail" });
          void api.getMode().then(show, () => undefined);
        });
    },
  });
  const apToggle = switchControl({
    checked: true,
    label: "Autopilot",
    onChange: (on) => void api.autopilotEnable(on).then(show).catch((e: unknown) => toast(errorText(e), { tone: "fail" })),
  });
  const shareToggle = switchControl({
    checked: true,
    label: "Ask to share my numbers",
    onChange: (on) =>
      void api
        .shareSetPrompt(on)
        .then((st) => shareToggle.setAttribute("aria-checked", String(st.enabled)))
        .catch((e: unknown) => toast(errorText(e), { tone: "fail" })),
  });
  void api.getShareStatus().then((st) => shareToggle.setAttribute("aria-checked", String(st.enabled)), () => undefined);
  void events.shareChanged((st) => shareToggle.setAttribute("aria-checked", String(st.enabled)), ctx.signal).catch(() => undefined);
  const notifyToggle = switchControl({
    checked: false,
    label: "Windows notifications",
    onChange: (on) =>
      void api
        .setNotifications(on)
        .then((n) => notifyToggle.setAttribute("aria-checked", String(n.enabled)))
        .catch((e: unknown) => {
          toast(errorText(e), { tone: "fail" });
          void api.getNotifications().then((n) => notifyToggle.setAttribute("aria-checked", String(n.enabled)), () => undefined);
        }),
  });
  void api.getNotifications().then((n) => notifyToggle.setAttribute("aria-checked", String(n.enabled)), () => undefined);
  const historyList = h("ul", { class: "autopilot-history" });
  const communityEl = h("p", { class: "fine" });
  const advanced = h(
    "details",
    { class: "advanced" },
    h("summary", {}, "Advanced"),
    h(
      "div",
      { class: "advanced-body" },
      h(
        "div",
        { class: "toggle-row" },
        h(
          "div",
          { class: "toggle-text" },
          h("p", { class: "toggle-label", text: "Autopilot" }),
          h("p", { class: "toggle-help", text: "After each game it reads the log and changes at most one thing for next time, within the mode's limits. Everything it does is listed here and can be undone." }),
        ),
        h("div", { class: "toggle-control" }, apToggle),
      ),
      historyList,
      h(
        "div",
        { class: "toggle-row" },
        h(
          "div",
          { class: "toggle-text" },
          h("p", { class: "toggle-label", text: "Ask to share my numbers" }),
          h("p", { class: "toggle-help", text: "After a game with a minute or more of play, Home offers to open a prefilled device report. Nothing is sent until you submit it on GitHub." }),
        ),
        h("div", { class: "toggle-control" }, shareToggle),
      ),
      h(
        "div",
        { class: "toggle-row" },
        h(
          "div",
          { class: "toggle-text" },
          h("p", { class: "toggle-label", text: "Windows notifications" }),
          h("p", { class: "toggle-help", text: "A Windows notification when shaders are ready or an update is installed, while the launcher is in the background." }),
        ),
        h("div", { class: "toggle-control" }, notifyToggle),
      ),
      communityEl,
      h(
        "div",
        { class: "row" },
        button("All settings", { kind: "secondary", size: "sm", icon: "sliders", onClick: () => void go("profile") }),
        button("Doctor", { kind: "ghost", size: "sm", icon: "pulse", onClick: () => void go("doctor") }),
      ),
    ),
  );

  function show(st: ModeStatus): void {
    if (ctx.signal.aborted) return;
    seg.set(st.mode === "custom" ? "" : st.mode);
    reason.textContent = st.reason;
    note.hidden = !st.note;
    note.textContent = st.note ?? "";
    const latest = st.autopilot.latest;
    autoRow.hidden = !latest || !st.autopilot.enabled;
    if (latest) auto.textContent = latest.text;
    undo.hidden = !(latest && latest.kind === "change");
    undo.disabled = !st.autopilot.can_undo;
    apToggle.setAttribute("aria-checked", String(st.autopilot.enabled));
    historyList.replaceChildren(
      ...(st.autopilot.history.length
        ? st.autopilot.history.map((c) =>
            h("li", { class: c.undone ? "is-undone" : "" }, h("span", { text: c.text.replace(/^Autopilot /, "") }), h("span", { class: "muted", text: ` ${relativeTime(c.at)}${c.undone ? ", undone" : ""}` })),
          )
        : [h("li", { class: "muted", text: "Nothing changed yet. Autopilot starts after your first full session." })]),
    );
    communityEl.textContent = communityLine(st);
  }

  undo.addEventListener("click", () => {
    void busy(undo, () => api.autopilotUndo(), "Undoing…")
      .then((st) => {
        show(st);
        toast("Undone. Autopilot leaves that setting alone until you change the mode.", { tone: "ok" });
      })
      .catch((e: unknown) => toast(errorText(e), { tone: "fail" }));
  });

  void api.getMode().then(show, (e: unknown) => reason.replaceChildren(`Mode unavailable: ${errorText(e)}`));
  void events.autopilotChanged((st) => show(st), ctx.signal).catch(() => undefined);

  return h(
    "section",
    { class: "card mode-card", "aria-label": "How Guarana runs" },
    h("div", { class: "mode-head" }, h("h2", { class: "card-title", text: "How should it run?" }), seg.el),
    reason,
    note,
    autoRow,
    advanced,
  );
}

// ---- Game Booster: Off / Level 1 / 2 / 3 --------------------------------------------------------------

function boosterCard(ctx: ScreenContext): HTMLElement {
  const group = h("div", { class: "booster-levels", role: "radiogroup", "aria-label": "Game Booster level" });
  const changes = h("ul", { class: "booster-changes" });
  const activeRow = h("p", { class: "booster-active", role: "status", hidden: true }, icon("bolt"), h("span", { text: "Boost is on for this game. It will be undone when the game exits." }));
  const note = h("p", { class: "fine mode-note", hidden: true });
  const undo = button("Undo all", { kind: "ghost", size: "sm", icon: "history", attrs: { title: "Put every change back right now" } });
  const levelName = h("span", { class: "disclosure-value", "aria-live": "polite" });
  let current = 0;

  const select = (level: number) => {
    if (level === current) return;
    void api
      .setBoosterLevel(level)
      .then(show)
      .catch((e: unknown) => {
        toast(errorText(e), { tone: "fail" });
        void api.getBooster().then(show, () => undefined);
      });
  };

  function show(st: BoosterStatus): void {
    if (ctx.signal.aborted) return;
    current = st.level;
    group.replaceChildren(
      ...st.levels.map((info) => {
        const row = h(
          "button",
          { type: "button", role: "radio", class: "booster-level", "aria-checked": String(info.level === current) },
          h(
            "span",
            { class: "booster-level-head" },
            h("span", { class: "booster-level-title", text: info.title }),
            info.elevated ? h("span", { class: "booster-tag" }, icon("shield"), "Needs a Windows prompt") : null,
          ),
          h("span", { class: "booster-level-summary", text: info.summary }),
        );
        row.addEventListener("click", () => select(info.level));
        return row;
      }),
    );
    const chosen = st.levels.find((l) => l.level === current);
    levelName.textContent = st.active ? `${chosen?.title ?? "On"} · on for this game` : (chosen?.title ?? "");
    changes.replaceChildren(
      ...(chosen && chosen.changes.length
        ? chosen.changes.map((c) => h("li", {}, icon("check"), h("span", { text: c })))
        : [h("li", { class: "muted" }, icon("info"), h("span", { text: "Nothing changes. Windows runs games as usual." }))]),
    );
    activeRow.hidden = !st.active;
    note.hidden = !st.note;
    note.textContent = st.note ?? "";
    // A note (what Undo or an exit did) is never left hidden inside the folded card.
    if (st.note) box.open = true;
  }

  undo.addEventListener("click", () => {
    void busy(undo, () => api.boosterUndoAll(), "Undoing…")
      .then((st) => {
        show(st);
        if (st.note) toast(st.note, { tone: "ok" });
      })
      .catch((e: unknown) => toast(errorText(e), { tone: "fail" }));
  });

  // Folded by default: one line with the current level; the four levels and their changes open on demand.
  const box = h(
    "details",
    { class: "card disclosure booster-card" },
    h(
      "summary",
      { class: "disclosure-head" },
      h("h3", { class: "card-title", text: "Game Booster" }),
      levelName,
      h("span", { class: "disclosure-chevron", "aria-hidden": "true" }, icon("chevronDown")),
    ),
    h(
      "div",
      { class: "disclosure-body" },
      h("p", { class: "fine", text: "Tunes Windows for speed only while a game runs, and puts everything back when it exits. Off by default. It never touches security settings." }),
      group,
      h("h4", { class: "booster-sub", text: "What this level changes" }),
      changes,
      activeRow,
      note,
      h("div", { class: "row" }, undo),
    ),
  );

  void api.getBooster().then(show, (e: unknown) => {
    levelName.textContent = "Unavailable";
    changes.replaceChildren(h("li", { class: "muted", text: `Booster unavailable: ${errorText(e)}` }));
  });
  void events.boosterChanged(show, ctx.signal).catch(() => undefined);

  return box;
}
