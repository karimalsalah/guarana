// Launch stage: the hero card has morphed into the full window. launch_game runs, then the stage
// dims to black over one second while the game window takes over. game://exited brings us back.

import { api, errorText, events } from "../api";
import { h } from "../dom";
import { exitCode } from "../format";
import { setPadSuspended } from "../gamepad";
import { focusFirst } from "../input";
import { reducedMotion, setPaused, wait } from "../motion";
import { go, type ScreenDef } from "../router";
import { recordSession } from "../sessions";
import type { Game, GameExited } from "../types";
import { button } from "../ui/controls";
import { heroArt } from "../ui/hero-art";

type Phase = "starting" | "handoff" | "playing" | "failed" | "done";

const screen: ScreenDef = {
  title: "Starting",
  chrome: "none",

  render(ctx) {
    const game = ctx.params.game as Game | undefined;
    const profileId = (ctx.params.profileId as string | null | undefined) ?? undefined;
    const art = heroArt({ cls: "launch-art" });
    const title = h("h1", { class: "launch-title", text: game?.title ?? "Game" });
    if (ctx.params.morph === true) {
      art.style.setProperty("view-transition-name", "hero-art");
      title.style.setProperty("view-transition-name", "hero-title");
    }
    const status = h("p", { class: "launch-status", role: "status", text: "Starting Guarana…" });
    const bar = h("div", { class: "launch-bar", "aria-hidden": "true" }, h("i"));
    const actions = h("div", { class: "launch-actions" });
    const blackout = h("div", { class: "blackout", "aria-hidden": "true" });
    const body = h("div", { class: "launch-body" }, h("p", { class: "eyebrow launch-eyebrow", text: "Starting" }), title, status, bar, actions);
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

    const fail = (message: string) => {
      phase = "failed";
      setPaused("game", false);
      setPadSuspended(false);
      el.classList.remove("is-starting", "is-handoff", "is-playing");
      el.classList.add("is-failed");
      body.querySelector(".launch-eyebrow")?.replaceChildren("Didn't start");
      status.textContent = message;
      actions.replaceChildren(
        button("Back to Home", { kind: "primary", size: "lg", primary: true, autofocus: true, onClick: () => void go("home", { transition: "back", reset: true }) }),
        button("Report a problem", { kind: "secondary", size: "lg", icon: "flag", onClick: () => void go("report", { params: { kind: "crash", note: message } }) }),
      );
      focusFirst(actions);
    };

    const onExit = (e: GameExited) => {
      if (!game || phase === "done" || phase === "failed") return;
      const early = phase === "starting" || phase === "handoff" || e.seconds < 10;
      setPaused("game", false);
      setPadSuspended(false);
      recordSession(game.title_id, e.seconds);
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

    void (async () => {
      // Listen first, so an exit right after start is never missed.
      try {
        await events.gameExited(onExit, ctx.signal);
      } catch (e) {
        fail(errorText(e));
        return;
      }
      setPadSuspended(true);
      try {
        await api.launchGame({ title_id: game.title_id, profile_id: profileId });
      } catch (e) {
        await entered;
        fail(errorText(e));
        return;
      }
      if (ctx.signal.aborted || phase !== "starting") return;
      await entered;
      if (phase !== "starting") return;
      phase = "handoff";
      status.textContent = "Handing over to the game window…";
      el.classList.replace("is-starting", "is-handoff");
      await wait(reducedMotion() ? 0 : 1000);
      if (phase !== "handoff") return;
      phase = "playing";
      el.classList.replace("is-handoff", "is-playing");
      const playing = h(
        "div",
        { class: "playing" },
        h("p", { class: "playing-title", text: `Playing ${game.title}` }),
        h("p", { class: "playing-hint", text: "Close the game to come back here." }),
      );
      body.replaceChildren(playing);
      requestAnimationFrame(() => requestAnimationFrame(() => playing.classList.add("is-shown")));
      // Nothing moves in the launcher while the game runs: it shares the GPU.
      setPaused("game", true);
    })();

    return el;
  },
};

export default screen;
