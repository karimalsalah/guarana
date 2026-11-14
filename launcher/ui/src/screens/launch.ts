// Launch stage: the interface has folded into the seed, which now sits alone on black. launch_game
// runs, and the stage follows the boot (game://boot): each phase blooms as a ring around the seed,
// and while shaders load their real count settles onto the third ring as sparks and rolls as a
// number beside the rings, with the time since Play. The status line says the phase in words. When the game is on screen the rings fall into the seed, the iris opens
// and its black takes the window: the game has it now. game://exited brings us back.

import { api, errorText, events } from "../api";
import { h } from "../dom";
import { exitCode } from "../format";
import { setPadSuspended } from "../gamepad";
import { focusFirst } from "../input";
import { isIdle, isPaused, onIdleChange, onPauseChange, reducedMotion, setPaused, wait } from "../motion";
import { go, type ScreenDef } from "../router";
import { recordSession } from "../sessions";
import type { BootProgress, Game, GameExited } from "../types";
import { icon } from "../icons";
import { button } from "../ui/controls";
import { odometer, paddedCount } from "../ui/numbers";
import { orbit, seed } from "../ui/seed";

type Phase = "starting" | "handoff" | "playing" | "failed" | "done";

// How long the stage holds without news before it hands over anyway (CONTRACT.md, "Boot progress
// and the hand-over").
/** Nothing heard at all: an emulator that writes no boot and whose window never took the foreground. */
const HOLD_QUIET_MS = 20_000;
/** The same for an emulator that writes its boot (follows_boot), before its window has the foreground:
 *  its first line comes after its own start-up, which a cold first start (a virus scan of the new
 *  build) can make long. */
const HOLD_QUIET_SESSION_MS = 45_000;
/** Its window has the foreground and it writes no boot (a version before the launcher session). */
const HOLD_WINDOW_ONLY_MS = 1_000;
/** Its window has the foreground and the boot goes on behind it: the next line resets this. */
const HOLD_BEHIND_MS = 60_000;
/** The first frame is presented; the emulator's loading screen fades out over 0.5 s after it. */
const HOLD_FIRST_FRAME_MS = 1_000;

const count = (n: number) => n.toLocaleString();

/** Elapsed time since Play as m:ss. */
function clock(ms: number): string {
  const s = Math.max(0, Math.floor(ms / 1000));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
}

/** The boot phase in plain words; null keeps the current line. */
function bootText(b: BootProgress): string | null {
  switch (b.phase) {
    case "starting":
      return "Starting Guarana…";
    case "loading":
      return "Loading the game…";
    case "shaders":
      return "Loading shaders…";
    case "precache":
      return "Preparing shaders ahead…";
    case "launching":
    case "first-frame":
    case "shown":
      return "Starting the game…";
    default:
      return null;
  }
}

/** Which ring a boot phase lights: 0 start, 1 load, 2 shaders, 3 the game itself. */
function ringOf(b: BootProgress): number | null {
  switch (b.phase) {
    case "starting":
      return 0;
    case "loading":
      return 1;
    case "shaders":
    case "precache":
      return 2;
    case "launching":
    case "first-frame":
    case "shown":
      return 3;
    default:
      return null;
  }
}

const PHASE_NAMES = ["Start", "Load", "Shaders", "Game"];

const screen: ScreenDef = {
  title: "Starting",
  chrome: "none",

  render(ctx) {
    const game = ctx.params.game as Game | undefined;
    const profileId = (ctx.params.profileId as string | null | undefined) ?? undefined;
    // A file opened from Explorer or a guarana:// link: started by its path, which may be outside the library.
    const byPath = ctx.params.byPath === true;

    const theSeed = seed({ size: "launch", state: "open" });
    const rings = orbit();
    theSeed.el.appendChild(rings.el);
    // It arrives as Home's seed left it: wide with interest. It relaxes once it has landed.
    theSeed.dilate(1.3);
    const title = h("h1", { class: "launch-title", text: game?.title ?? "Game" });
    // From Home the seed and the title both fly here; from a library row the title always does.
    if (ctx.params.morph === true) theSeed.el.style.setProperty("view-transition-name", "seed");
    if (ctx.params.morph === true || ctx.params.morph === "title") title.style.setProperty("view-transition-name", "hero-title");

    // One line of what is happening: the boot phase in words (game://boot), and while shaders load
    // their real count after it. The count changes every percent, so it sits outside the live
    // region: screen readers hear the phases, not every number. The rings are the same story drawn.
    const statusText = h("span", { role: "status", text: "Starting Guarana…" });
    const countText = h("span", { class: "launch-count" });
    const status = h("p", { class: "launch-status" }, statusText, countText);
    const phaseItems = PHASE_NAMES.map((name) => h("li", { class: "launch-phase" }, h("span", { class: "launch-phase-dot" }), name));
    const phases = h("ol", { class: "launch-phases", "aria-hidden": "true" }, phaseItems);
    const actions = h("div", { class: "launch-actions" });
    const eyebrow = h("p", { class: "eyebrow launch-eyebrow", text: "Starting" });
    const body = h("div", { class: "launch-body" }, eyebrow, title, status, phases, actions);
    // Beside the rings while shaders load: the same real count as a rolling number, and the time
    // since Play. It repeats the status line (which is what screen readers get), so it is hidden
    // from them.
    const t0 = performance.now();
    const tallyCount = odometer("0", { cls: "tally-odo", lead: true });
    const tallyTotal = h("span", { class: "tally-total" });
    const tallyClock = h("span", { class: "tally-clock", text: "0:00" });
    const tally = h(
      "div",
      { class: "launch-tally", "aria-hidden": "true" },
      h("span", { class: "tally-count" }, tallyCount.el),
      h("span", { class: "tally-of" }, tallyTotal),
      h("span", { class: "tally-time" }, h("span", { class: "tally-time-label", text: "since Play" }), tallyClock),
    );
    let clockTimer = 0;
    const tick = () => {
      tallyClock.textContent = clock(performance.now() - t0);
    };
    // One text update a second, only while the count shows, nothing is paused and the window has
    // focus. Back from a pause or idle, the first tick catches the text up.
    const runClock = () => {
      window.clearInterval(clockTimer);
      clockTimer = 0;
      if (tally.classList.contains("is-on") && !tally.classList.contains("is-done") && !isPaused() && !isIdle()) {
        tick();
        clockTimer = window.setInterval(tick, 1000);
      }
    };
    const offPause = onPauseChange(() => runClock());
    const offIdle = onIdleChange(() => runClock());
    ctx.signal.addEventListener("abort", () => {
      window.clearInterval(clockTimer);
      offPause();
      offIdle();
    });
    const hideTally = () => {
      tally.classList.remove("is-on");
      runClock();
    };

    // The iris: a black disc on the seed's pupil that opens until it holds the whole window.
    const iris = h("div", { class: "seed-void", "aria-hidden": "true" });
    const el = h("div", { class: "launch is-starting" }, h("div", { class: "launch-glow", "aria-hidden": "true" }), h("div", { class: "launch-seed" }, theSeed.el, tally), body, iris);

    let phase: Phase = "starting";
    let ringIndex = -1;
    const showRing = (i: number) => {
      if (i < ringIndex) return;
      ringIndex = i;
      rings.phase(i);
      phaseItems.forEach((li, j) => {
        li.classList.toggle("is-done", j < i);
        li.classList.toggle("is-now", j === i);
      });
    };

    let enteredResolve: () => void = () => undefined;
    const entered = new Promise<void>((resolve) => {
      enteredResolve = () => resolve();
    });
    ctx.onEntered(() => {
      theSeed.el.style.removeProperty("view-transition-name");
      title.style.removeProperty("view-transition-name");
      if (phase === "starting") {
        theSeed.dilate(1);
        showRing(Math.max(0, ringIndex));
      }
      enteredResolve();
    });
    // Back and tab switching do nothing while the game starts or runs.
    ctx.onIntent((intent) => phase !== "failed" && (intent === "back" || intent === "tabPrev" || intent === "tabNext" || intent === "primary"));
    ctx.onLeave(() => {
      setPadSuspended(false);
      setPaused("game", false);
    });

    // game://boot: the stage holds until the game is on screen, and hands over anyway after the
    // HOLD_* times without news, as it did before the emulator reported its boot.
    let launched = false;
    let followsBoot = false; // launch_game: this emulator writes its boot
    let sawBoot = false; // the emulator writes its boot (a launcher session)
    let sawWindow = false; // a window of the emulator has had the foreground
    let lastBoot: BootProgress["phase"] | null = null;
    let holdTimer: ReturnType<typeof setTimeout> | undefined;
    ctx.signal.addEventListener("abort", () => clearTimeout(holdTimer));

    const showBoot = (b: BootProgress) => {
      const text = bootText(b);
      if (text) statusText.textContent = text;
      const ring = ringOf(b);
      if (ring !== null) showRing(ring);
      const counted = (b.phase === "shaders" || b.phase === "precache") && b.total > 0;
      countText.textContent = counted ? `${count(b.done)} of ${count(b.total)}` : "";
      if (counted) {
        rings.particles(b.done, b.total);
        tallyCount.set(paddedCount(b.done, b.total));
        tallyTotal.textContent = `of ${count(b.total)} shaders`;
        tick();
        if (!tally.classList.contains("is-on")) {
          tally.classList.add("is-on");
          runClock();
        }
      } else if (ring !== null && ring > 2 && tally.classList.contains("is-on") && !tally.classList.contains("is-done")) {
        // Shaders are in: the count settles in leaf and the clock stops at how long they took.
        tick();
        tally.classList.add("is-done");
        window.clearInterval(clockTimer);
        clockTimer = 0;
      }
    };

    const fail = (message: string) => {
      clearTimeout(holdTimer);
      countText.textContent = "";
      hideTally();
      phase = "failed";
      setPaused("game", false);
      setPadSuspended(false);
      el.classList.remove("is-starting", "is-handoff", "is-playing");
      el.classList.add("is-failed");
      // The fruit closes and the rings go dark: nothing is spinning for a launch that is over.
      rings.fail();
      theSeed.setState("shut");
      theSeed.dilate(1);
      eyebrow.replaceChildren(icon("fail"), "Didn't start");
      statusText.textContent = message;
      actions.replaceChildren(
        button("Back to Home", { kind: "primary", size: "lg", primary: true, autofocus: true, onClick: () => void go("home", { transition: "back", reset: true }) }),
        button("Report a problem", { kind: "secondary", size: "lg", icon: "flag", onClick: () => void go("report", { params: { kind: "crash", note: message } }) }),
      );
      focusFirst(actions);
    };

    const onExit = (e: GameExited) => {
      if (!game || phase === "done" || phase === "failed") return;
      clearTimeout(holdTimer);
      const early = phase === "starting" || phase === "handoff" || e.seconds < 10;
      setPaused("game", false);
      setPadSuspended(false);
      if (game.title_id) recordSession(game.title_id, e.seconds);
      if (e.code !== 0 && early) {
        iris.classList.remove("is-on", "is-open");
        iris.style.transform = "";
        fail(`${game.title} closed while starting (exit code ${exitCode(e.code)}). The log is in the report.`);
        return;
      }
      phase = "done";
      void go("home", { transition: "return", reset: true, params: { exited: { code: e.code, seconds: e.seconds, title: game.title } } });
    };

    if (!game) {
      setTimeout(() => fail("Nothing to start."), 0);
      return el;
    }

    /** The iris opens: a black disc grows from the pupil until it covers the window (one transform). */
    const openIris = () => {
      const p = theSeed.pupil.getBoundingClientRect();
      const d = Math.max(8, p.width);
      const cx = p.left + p.width / 2;
      const cy = p.top + p.height / 2;
      const far = Math.hypot(Math.max(cx, innerWidth - cx), Math.max(cy, innerHeight - cy));
      iris.style.left = `${(cx - d / 2).toFixed(1)}px`;
      iris.style.top = `${(cy - d / 2).toFixed(1)}px`;
      iris.style.width = iris.style.height = `${d.toFixed(1)}px`;
      iris.classList.add("is-on");
      void iris.offsetWidth;
      // The disc's solid core is 56 % of its radius; the rest is a soft edge that sweeps outward.
      iris.style.transform = `scale(${((far * 2) / (0.56 * d)).toFixed(2)})`;
      iris.classList.add("is-open");
    };

    const handOver = async () => {
      if (ctx.signal.aborted || phase !== "starting") return;
      clearTimeout(holdTimer);
      const quick = reducedMotion() || sawWindow;
      // Everything falls into the seed and it opens wide.
      showRing(3);
      rings.release();
      theSeed.setState("wide");
      theSeed.dilate(2.05);
      countText.textContent = "";
      hideTally();
      if (!sawWindow) {
        // Behind the game's window this would play unseen: it only waits when the stage is in front.
        phase = "handoff";
        statusText.textContent = "Handing over to the game window…";
        el.classList.replace("is-starting", "is-handoff");
      }
      await wait(quick ? 0 : 420);
      if (phase !== "handoff" && phase !== "starting") return;
      openIris();
      await wait(quick ? 0 : 640);
      if (phase !== "handoff" && phase !== "starting") return;
      phase = "playing";
      el.classList.remove("is-starting", "is-handoff");
      el.classList.add("is-playing");
      // "Running", not "Playing": after a HOLD_* time without news the launcher knows only that the
      // process started, not that a frame is up.
      const playing = h(
        "div",
        { class: "playing" },
        h("p", { class: "playing-title", text: `${game.title} is running` }),
        h("p", { class: "playing-hint", text: "It opens in its own window. Close the game to come back here." }),
      );
      body.replaceChildren(playing);
      requestAnimationFrame(() => requestAnimationFrame(() => playing.classList.add("is-shown")));
      // Nothing moves in the launcher while the game runs: it shares the GPU.
      setPaused("game", true);
    };

    // The next hand-over deadline, from what the boot has told so far.
    const schedule = () => {
      if (!launched || phase !== "starting") return;
      clearTimeout(holdTimer);
      const ms =
        lastBoot === "shown"
          ? 0
          : lastBoot === "first-frame"
            ? HOLD_FIRST_FRAME_MS
            : sawWindow
              ? sawBoot
                ? HOLD_BEHIND_MS
                : HOLD_WINDOW_ONLY_MS
              : followsBoot
                ? HOLD_QUIET_SESSION_MS
                : HOLD_QUIET_MS;
      holdTimer = setTimeout(() => void handOver(), ms);
    };

    const onBoot = (b: BootProgress) => {
      if (phase !== "starting") return;
      if (b.phase === "window") {
        sawWindow = true;
        // The emulator's window covers the stage now: nothing here may take GPU time from it.
        setPaused("game", true);
      } else {
        sawBoot = true;
        lastBoot = b.phase;
        showBoot(b);
      }
      schedule();
    };

    void (async () => {
      // Listen first, so an exit right after start is never missed.
      try {
        await events.gameExited(onExit, ctx.signal);
      } catch (e) {
        fail(errorText(e));
        return;
      }
      // Without boot events the stage falls back to the HOLD_QUIET_MS hand-over.
      await events.gameBoot(onBoot, ctx.signal).catch(() => undefined);
      setPadSuspended(true);
      try {
        const started = await api.launchGame(
          byPath
            ? { title_id: game.title_id || undefined, path: game.path, profile_id: profileId }
            : { title_id: game.title_id, profile_id: profileId },
        );
        followsBoot = started.follows_boot === true;
      } catch (e) {
        await entered;
        fail(errorText(e));
        return;
      }
      if (ctx.signal.aborted || phase !== "starting") return;
      await entered;
      if (phase !== "starting") return;
      // Hold the stage until the game is on screen (or a HOLD_* time passes without news).
      launched = true;
      schedule();
    })();

    return el;
  },
};

export default screen;
