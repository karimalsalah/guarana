// Launch stage: the hero card has morphed into the full window. launch_game runs, and the stage
// follows the boot (game://boot: the phase in words, the shader counts as a real bar) until the
// game is on screen, then gives way to "Playing". game://exited brings us back.

import { api, errorText, events } from "../api";
import { h } from "../dom";
import { exitCode } from "../format";
import { setPadSuspended } from "../gamepad";
import { focusFirst } from "../input";
import { reducedMotion, setPaused, wait } from "../motion";
import { go, type ScreenDef } from "../router";
import { recordSession } from "../sessions";
import type { BootProgress, Game, GameExited } from "../types";
import { icon } from "../icons";
import { button } from "../ui/controls";
import { heroArt } from "../ui/hero-art";

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

const screen: ScreenDef = {
  title: "Starting",
  chrome: "none",

  render(ctx) {
    const game = ctx.params.game as Game | undefined;
    const profileId = (ctx.params.profileId as string | null | undefined) ?? undefined;
    // A file opened from Explorer or a guarana:// link: started by its path, which may be outside the library.
    const byPath = ctx.params.byPath === true;
    const art = heroArt({ cls: "launch-art" });
    const title = h("h1", { class: "launch-title", text: game?.title ?? "Game" });
    if (ctx.params.morph === true) {
      art.style.setProperty("view-transition-name", "hero-art");
      title.style.setProperty("view-transition-name", "hero-title");
    } else if (ctx.params.morph === "title") {
      // From a library row only the title morphs (that page has no hero art).
      title.style.setProperty("view-transition-name", "hero-title");
    }
    const status = h("p", { class: "launch-status", role: "status", text: "Starting Guarana…" });
    // The counts change every percent: outside the live region, so screen readers hear the phases.
    const detail = h("p", { class: "launch-detail" });
    const bar = h("div", { class: "launch-bar", "aria-hidden": "true" }, h("i"));
    const actions = h("div", { class: "launch-actions" });
    const blackout = h("div", { class: "blackout", "aria-hidden": "true" });
    const eyebrow = h("p", { class: "eyebrow launch-eyebrow", text: "Starting" });
    const body = h("div", { class: "launch-body" }, eyebrow, title, status, detail, bar, actions);
    const el = h("div", { class: "launch is-starting" }, art, body, blackout);

    let phase: Phase = "starting";
    let enteredResolve: () => void = () => undefined;
    const entered = new Promise<void>((resolve) => {
      enteredResolve = () => resolve();
    });
    ctx.onEntered(() => {
      art.style.removeProperty("view-transition-name");
      title.style.removeProperty("view-transition-name");
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
      if (text) status.textContent = text;
      const counted = (b.phase === "shaders" || b.phase === "precache") && b.total > 0;
      detail.textContent = counted ? `${count(b.done)} of ${count(b.total)}` : "";
      bar.classList.toggle("is-determinate", counted);
      if (counted) bar.style.setProperty("--done", `${((b.done / b.total) * 100).toFixed(1)}%`);
    };

    const fail = (message: string) => {
      clearTimeout(holdTimer);
      detail.textContent = "";
      phase = "failed";
      setPaused("game", false);
      setPadSuspended(false);
      el.classList.remove("is-starting", "is-handoff", "is-playing");
      el.classList.add("is-failed");
      eyebrow.replaceChildren(icon("fail"), "Didn't start");
      status.textContent = message;
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

    const handOver = async () => {
      if (ctx.signal.aborted || phase !== "starting") return;
      clearTimeout(holdTimer);
      // Behind the game's window the dimming would play unseen: straight to "Playing" then.
      if (!sawWindow) {
        phase = "handoff";
        status.textContent = "Handing over to the game window…";
        detail.textContent = "";
        el.classList.replace("is-starting", "is-handoff");
        await wait(reducedMotion() ? 0 : 1000);
        if (phase !== "handoff") return;
      }
      phase = "playing";
      el.classList.remove("is-starting", "is-handoff");
      el.classList.add("is-playing");
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
