// Entry point: theme, motion policy, chrome, input, then the first screen.

import { api, errorText, events, inTauri } from "./api";
import { h } from "./dom";
import { installText } from "./format";
import { activeKind, initGamepad, syncPadAttribute } from "./gamepad";
import { icon } from "./icons";
import { configureIntents, initInput } from "./input";
import { initLaunchRequests } from "./launch-requests";
import { initMotion, setPaused } from "./motion";
import { back, cycleTab, go, preload, type ScreenId } from "./router";
import { hideBoot, mountShell, refreshVersions } from "./shell";
import { controllers, initControllers, initWarm, launcher } from "./state";
import { initTheme } from "./theme";

initTheme();
initMotion();
document.documentElement.toggleAttribute("data-mock", !inTauri);

const app = document.getElementById("app") as HTMLElement;
const shell = mountShell(app, () => void go("report"));
initInput();
configureIntents({ back, tab: cycleTab });
initGamepad();
initControllers();
initWarm();

const syncPad = () => syncPadAttribute(controllers.value.filter((c) => c.connected).map((c) => c.kind));
controllers.subscribe(syncPad);
activeKind.subscribe(syncPad);

// Links never navigate the webview: https opens in the default browser, the rest is ignored.
document.addEventListener("click", (e) => {
  if (e.defaultPrevented) return;
  const target = e.target as Element | null;
  const a = target?.closest<HTMLAnchorElement>("a[href]");
  if (!a) return;
  const href = a.getAttribute("href") ?? "";
  if (href.startsWith("#")) return;
  e.preventDefault();
  if (/^https:\/\//i.test(href)) void api.openUrl(href).catch(() => undefined);
});

// Safety net: whatever screen is showing, a game exit resumes the launcher's animations and input.
void events.gameExited(() => setPaused("game", false)).catch(() => undefined);

// Screens dispatch this after changing the installed version, so the footer stays right.
document.addEventListener("guarana:state", () => void refreshVersions(shell));

void boot();

async function boot(): Promise<void> {
  const q = new URLSearchParams(location.search);
  const requested = q.get("screen");
  const allowed: ScreenId[] = ["welcome", "scan", "profile", "home", "updates", "doctor", "report"];
  // On a first start get_state waits while the backend installs the build the installer carries;
  // its install://progress shows under the boot mark.
  const bootStatus = document.querySelector<HTMLElement>("#boot .boot-status");
  const unlisten = await events
    .installProgress((p) => {
      if (!bootStatus) return;
      bootStatus.replaceChildren(h("strong", { text: "Setting up Guarana for the first time" }), installText(p));
    })
    .catch(() => null);
  let first: ScreenId;
  try {
    const state = await launcher.get();
    first = state.first_run ? "welcome" : "home";
  } catch (e) {
    unlisten?.();
    bootError(errorText(e));
    hideBoot();
    return;
  }
  unlisten?.();
  if (requested && (allowed as string[]).includes(requested)) first = requested as ScreenId;
  if (first === "welcome") preload("scan");
  try {
    await go(first, { transition: "none" });
  } catch (e) {
    bootError(errorText(e));
  }
  hideBoot();
  void refreshVersions(shell);
  // A game file opened from Explorer or a guarana:// link, now or later.
  initLaunchRequests();
}

function bootError(message: string): void {
  document.documentElement.dataset.chrome = "none";
  shell.outlet.replaceChildren(
    h(
      "section",
      { class: "screen boot-error" },
      h("div", { class: "boot-error-card" },
        h("span", { class: "boot-error-icon" }, icon("warn")),
        h("h1", { text: "The launcher couldn't start" }),
        h("p", { class: "lede", text: "It couldn't read its own settings. Your games and files are untouched." }),
        h("pre", { class: "boot-error-detail", text: message }),
        h("div", { class: "row" },
          h("button", { type: "button", class: "btn btn-primary btn-md", "data-autofocus": true, onClick: () => location.reload() }, h("span", { class: "btn-label", text: "Try again" })),
          h("button", { type: "button", class: "btn btn-secondary btn-md", onClick: () => void navigator.clipboard?.writeText(message).catch(() => undefined) }, h("span", { class: "btn-label", text: "Copy the error" })),
        ),
      ),
    ),
  );
}
