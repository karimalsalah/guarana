// Games asked for from outside the window: a file Explorer opened with Guarana, or a guarana://
// link. The backend checks each one (open.rs) and keeps the newest until it is taken here; the
// launch screen then starts it by its path through launch_game, like a game from the library.

import { api, events } from "./api";
import { currentScreen, go } from "./router";
import { launcher } from "./state";
import { toast } from "./ui/toast";

let started = false;

/** Call once the first screen shows: takes a request waiting from the start, then each new one. */
export function initLaunchRequests(): void {
  if (started) return;
  started = true;
  void events.launchRequest(() => void claim()).catch(() => undefined);
  void claim();
}

async function claim(): Promise<void> {
  const request = await api.takeLaunchRequest().catch(() => null);
  if (!request) return;
  const game = request.game;
  if (!game) {
    toast(request.error ?? "That game can't be started.", { tone: "fail" });
    return;
  }
  // A launch screen that says "Didn't start" may be left; one that starts or runs a game may not.
  if (currentScreen() === "launch" && !document.querySelector(".launch.is-failed")) {
    toast(`A game is already starting or running. Close it, then open ${game.title} again.`, { tone: "warn" });
    return;
  }
  const state = await launcher.get(true).catch(() => null);
  if (!state || state.first_run) {
    toast(`Finish setting up Guarana, then open ${game.title} again.`, { tone: "warn" });
    return;
  }
  if (!state.keys_present) {
    toast("Guarana needs your keys before a game can start.", { tone: "warn", action: { label: "Find my keys", run: () => void go("scan") } });
    return;
  }
  await go("launch", { params: { game, byPath: true }, transition: "fade" });
}
