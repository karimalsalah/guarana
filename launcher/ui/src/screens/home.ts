// Home, Ocarina-first: a hero card for Ocarina of Time with one big Play, the controller and setup
// cards, then the rest of the library (lazy). Warms the shader cache in the background once.

import { api, errorText, events } from "../api";
import { h } from "../dom";
import { duration, exitCode, relativeTime, v } from "../format";
import { rumble } from "../gamepad";
import { icon } from "../icons";
import { settleFocus } from "../input";
import { whenIdle } from "../motion";
import { go, type ScreenContext, type ScreenDef } from "../router";
import { lastSession } from "../sessions";
import { launcher, library, profiles, warmStatus } from "../state";
import type { BoosterStatus, Game, LauncherState, ModeId, ModeStatus, Profile, ShareStatus, UpdateInfo, WarmResult } from "../types";
import { badge, busy, button, card, segmented, skeleton, spinner, switchControl } from "../ui/controls";
import { controllersCard } from "../ui/controllers-card";
import { heroArt } from "../ui/hero-art";
import { toast, type Tone } from "../ui/toast";

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
      return { text: "Shaders were already prepared.", tone: "ok" };
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
    const heroSlot = h("div", { class: "hero-slot" }, heroPlaceholder());
    const setupSlot = h("div", { class: "setup-slot" }, card("Your setup", { icon: "shield" }, skeleton([80, 60, 70])));
    const libTitle = h("h2", { class: "section-title", id: "lib-title", text: "Library" });
    const libBody = h("div", { class: "library-body" }, skeleton([55, 40, 48], "skeleton-tall"));
    const libSection = h("section", { class: "library", "aria-labelledby": "lib-title" }, h("div", { class: "section-head" }, libTitle), libBody);

    const modeSlot = h("div", { class: "mode-slot" });
    const boosterSlot = h("div", { class: "booster-slot" });
    const shareSlot = shareCard(ctx);
    const el = h("div", { class: "home" }, heroSlot, shareSlot, modeSlot, boosterSlot, h("div", { class: "home-grid" }, controllersCard(ctx.signal), setupSlot), libSection);

    const exited = ctx.params.exited as { code: number; seconds: number; title: string } | undefined;
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
        if (!ctx.signal.aborted) heroSlot.replaceChildren(errorHero(errorText(e)));
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

      heroSlot.replaceChildren(renderHero(ctx, state, oot, profile, profileId));
      if (state.keys_present || state.profile_id) {
        modeSlot.replaceChildren(modeCard(ctx));
        boosterSlot.replaceChildren(boosterCard(ctx));
      }
      setupSlot.replaceChildren(setupCard(state, profile, ctx));
      settleFocus(el);

      const rest = oot && state.keys_present ? games.filter((g) => g !== oot) : games;
      libTitle.textContent = oot && state.keys_present ? "More games" : "Library";
      renderLibrary(libSection, libBody, rest, state, profileId, ctx);
    })();

    return el;
  },
};

export default screen;

function heroPlaceholder(): HTMLElement {
  return h(
    "article",
    { class: "hero is-loading", "aria-busy": "true", "aria-label": "Loading your library" },
    heroArt({ cls: "hero-art" }),
    h("div", { class: "hero-body" }, h("div", { class: "skeleton hero-skeleton" }, h("span", { class: "skeleton-line" }), h("span", { class: "skeleton-line" }), h("span", { class: "skeleton-line" }))),
  );
}

function errorHero(message: string): HTMLElement {
  return h(
    "article",
    { class: "hero hero-error" },
    heroArt({ cls: "hero-art" }),
    h(
      "div",
      { class: "hero-body" },
      h("p", { class: "eyebrow", text: "Something's off" }),
      h("h1", { class: "hero-title hero-title-sm", text: "Your library didn't load" }),
      h("p", { class: "hero-meta", text: message }),
      h("div", { class: "hero-actions" }, button("Try again", { kind: "primary", size: "lg", icon: "refresh", primary: true, autofocus: true, onClick: () => void go("home", { replace: true, transition: "fade" }) })),
    ),
  );
}

function renderHero(ctx: ScreenContext, state: LauncherState, oot: Game | undefined, profile: Profile | undefined, profileId: string | null): HTMLElement {
  const art = heroArt({ cls: "hero-art" });

  if (!state.keys_present) {
    return h(
      "article",
      { class: "hero hero-pending", "aria-labelledby": "hero-title" },
      art,
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
    return h(
      "article",
      { class: "hero hero-pending", "aria-labelledby": "hero-title" },
      art,
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

  const title = h("h1", { class: "hero-title", id: "hero-title", text: oot.title });
  const session = lastSession(oot.title_id);
  const meta = session ? `Last played ${relativeTime(session.last)} · ${duration(session.seconds)}` : "Ready for your first session";
  const chips = h(
    "ul",
    { class: "chips", "aria-label": "Profile" },
    h("li", { class: "chip" }, icon("sliders"), profile ? profile.name : "No profile yet"),
    HIGHLIGHTS.filter((x) => toggleOn(profile, x.id))
      .slice(0, 3)
      .map((x) => h("li", { class: "chip" }, icon("sparkle"), x.label)),
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
      art.style.setProperty("view-transition-name", "hero-art");
      title.style.setProperty("view-transition-name", "hero-title");
      void go("launch", { params: { game: oot, profileId, morph: true }, transition: "launch" }).catch((e: unknown) => {
        art.style.removeProperty("view-transition-name");
        title.style.removeProperty("view-transition-name");
        toast(errorText(e), { tone: "fail" });
      });
    },
  });

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
    art,
    h(
      "div",
      { class: "hero-body" },
      h("p", { class: "eyebrow", text: "Ready to play" }),
      title,
      h("p", { class: "hero-meta" }, icon("clock"), meta),
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
    if (ctx.signal.aborted || !info?.update_available) return;
    versionRow.appendChild(h("button", { type: "button", class: "link-btn", onClick: () => void go("updates") }, `${v(info.latest)} is out`, icon("chevronRight")));
    document.querySelector('.tab[data-tab="updates"]')?.classList.add("has-dot");
  });

  return card("Your setup", { icon: "shield" }, rows);
}

function renderLibrary(section: HTMLElement, body: HTMLElement, games: Game[], state: LauncherState, profileId: string | null, ctx: ScreenContext): void {
  if (!games.length) {
    body.replaceChildren(
      h(
        "div",
        { class: "empty" },
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
          onPlay: (g) => void go("launch", { params: { game: g, profileId }, transition: "fade" }),
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
  const share = button("Share", { kind: "primary", icon: "sparkle", onClick: (_e, self) => void busy(self, shareNow, "Opening…").catch((e: unknown) => toast(errorText(e), { tone: "fail" })) });
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
    changes.replaceChildren(
      ...(chosen && chosen.changes.length
        ? chosen.changes.map((c) => h("li", {}, icon("check"), h("span", { text: c })))
        : [h("li", { class: "muted" }, icon("info"), h("span", { text: "Nothing changes. Windows runs games as usual." }))]),
    );
    activeRow.hidden = !st.active;
    note.hidden = !st.note;
    note.textContent = st.note ?? "";
  }

  undo.addEventListener("click", () => {
    void busy(undo, () => api.boosterUndoAll(), "Undoing…")
      .then((st) => {
        show(st);
        if (st.note) toast(st.note, { tone: "ok" });
      })
      .catch((e: unknown) => toast(errorText(e), { tone: "fail" }));
  });

  void api.getBooster().then(show, (e: unknown) => changes.replaceChildren(h("li", { class: "muted", text: `Booster unavailable: ${errorText(e)}` })));
  void events.boosterChanged(show, ctx.signal).catch(() => undefined);

  return h(
    "section",
    { class: "card booster-card", "aria-label": "Game Booster" },
    h("div", { class: "mode-head" }, h("h2", { class: "card-title", text: "Game Booster" })),
    h("p", { class: "fine", text: "Tunes Windows for speed only while a game runs, and puts everything back when it exits. Off by default. It never touches security settings." }),
    group,
    h("h3", { class: "booster-sub", text: "What this level changes" }),
    changes,
    activeRow,
    note,
    h("div", { class: "row" }, undo),
  );
}
