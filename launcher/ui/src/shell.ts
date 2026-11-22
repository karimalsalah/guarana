// App chrome: everything that is not the stage recedes to a thin rail (the seed's mark, the tabs,
// the theme) and a hint bar (versions, project links, button hints). Plus the outlet, toasts and
// the live region.

import { AI_QUICKSTART_URL, DISCORD_URL } from "./ui/ask-ai";
import { api, inTauri } from "./api";
import { $$, h } from "./dom";
import { activeKind, glyph } from "./gamepad";
import { icon, type IconName } from "./icons";
import { onInputMode } from "./input";
import { animate } from "./motion";
import { attachShell, go, goTab, TABS, type Chrome, type TabId } from "./router";
import { launcher } from "./state";
import { currentTheme, onThemeChange, setTheme } from "./theme";
import { v } from "./format";
import { seed } from "./ui/seed";

const GITHUB = "https://github.com/karimalsalah/guarana";
const DISCUSSIONS = "https://github.com/karimalsalah/guarana/discussions";

const TAB_ICONS: Record<TabId, IconName> = { home: "home", profile: "sliders", updates: "download", doctor: "pulse" };

export interface Shell {
  outlet: HTMLElement;
  setVersions(launcherVersion: string | null, emulatorVersion: string | null): void;
}

export function mountShell(app: HTMLElement, onReport: () => void): Shell {
  // ---- Rail
  const tabButtons = new Map<TabId, HTMLButtonElement>();
  const indicator = h("span", { class: "tab-indicator", "aria-hidden": "true" });
  const tabList = h(
    "div",
    { class: "tabs-list" },
    indicator,
    TABS.map((t) => {
      const b = h(
        "button",
        { class: "tab", type: "button", "data-tab": t.id, onClick: () => goTab(t.id) },
        h("span", { class: "tab-icon" }, icon(TAB_ICONS[t.id])),
        h("span", { class: "tab-label", text: t.label }),
      );
      tabButtons.set(t.id, b);
      return b;
    }),
  );
  const tabs = h("nav", { class: "tabs", "aria-label": "Main" }, h("span", { class: "tab-hint" }, glyph("tabPrev")), tabList, h("span", { class: "tab-hint" }, glyph("tabNext")));

  // First run is one screen (Scan, then Home in Auto mode), so the rail says "Set up" and counts nothing.
  const steps = h("div", { class: "setup-steps", hidden: true }, h("span", { class: "steps-kicker", text: "Set up" }));

  const themeBtn = h("button", { class: "icon-btn theme-toggle", type: "button" });
  const syncThemeBtn = () => {
    const light = currentTheme() === "light";
    themeBtn.replaceChildren(icon(light ? "moon" : "sun"));
    themeBtn.setAttribute("aria-label", light ? "Switch to the dark theme" : "Switch to the light theme");
    themeBtn.title = light ? "Rainforest night (dark)" : "Aril (light)";
  };
  syncThemeBtn();
  onThemeChange(syncThemeBtn);
  themeBtn.addEventListener("click", () => {
    const r = themeBtn.getBoundingClientRect();
    void setTheme(currentTheme() === "light" ? "dark" : "light", { x: r.left + r.width / 2, y: r.top + r.height / 2 });
  });

  // The seed's mark watches focus on every screen: the interface's one living thing, in miniature.
  const mark = seed({ size: "mark", follow: true, cls: "brand-mark" });
  const rail = h(
    "header",
    { class: "rail" },
    h("div", { class: "brand" }, mark.el, h("span", { class: "brand-name", text: "Guarana" })),
    tabs,
    steps,
    h("div", { class: "rail-end" }, themeBtn),
  );

  // ---- Main
  const outlet = h("div", { class: "outlet" });
  const loadingBar = h("div", { class: "loading-bar", "aria-hidden": "true" });
  const main = h("main", { id: "main", tabindex: "-1" }, outlet);

  // ---- Hint bar
  const versions = h("span", { class: "footer-versions" });
  const link = (label: string, url: string) =>
    h("a", { href: url, class: "footer-link", onClick: (e: Event) => { e.preventDefault(); void api.openUrl(url).catch(() => undefined); } }, label);
  const padHints = h(
    "div",
    { class: "pad-hints", "aria-hidden": "true" },
    h("span", { class: "pad-hint" }, glyph("confirm"), "Select"),
    h("span", { class: "pad-hint" }, glyph("back"), "Back"),
    h("span", { class: "pad-hint" }, glyph("tabPrev"), glyph("tabNext"), "Tabs"),
  );
  const footer = h(
    "footer",
    { class: "hintbar" },
    versions,
    inTauri ? null : h("span", { class: "badge badge-neutral", title: "No launcher backend: everything here is sample data" }, "Sample data"),
    h(
      "nav",
      { class: "footer-links", "aria-label": "Project" },
      link("GitHub", GITHUB),
      link("Discussions", DISCUSSIONS),
      h(
        "details",
        { class: "help-menu" },
        h("summary", { class: "footer-link" }, "Help"),
        h(
          "div",
          { class: "help-pop" },
          link("Join the Discord", DISCORD_URL),
          link("Contribute with your AI agent", AI_QUICKSTART_URL),
          link("Read the docs", "https://karimalsalah.github.io/guarana/"),
        ),
      ),
      h("button", { type: "button", class: "footer-link", onClick: () => void go("about") }, "Credits"),
      h("button", { type: "button", class: "footer-link", onClick: onReport }, "Report a problem"),
    ),
    padHints,
  );

  const toasts = h("div", { class: "toasts", role: "region", "aria-label": "Notifications" });
  const announcer = h("div", { class: "sr-only", "aria-live": "polite", "aria-atomic": "true" });

  app.replaceChildren(rail, loadingBar, main, footer, toasts, announcer);

  // ---- Router hooks
  // The rail's marker is a leaf-green drop that slides between tabs and stretches on the way, like
  // the focus halo: one physical language for "where you are" and "what you are on".
  let markerTop = -1;
  const moveIndicator = (tab: TabId | undefined) => {
    for (const [id, b] of tabButtons) {
      if (id === tab) b.setAttribute("aria-current", "page");
      else b.removeAttribute("aria-current");
    }
    const b = tab ? tabButtons.get(tab) : undefined;
    if (!b) {
      indicator.style.opacity = "0";
      markerTop = -1;
      return;
    }
    const top = b.offsetTop + b.offsetHeight / 2 - 14;
    // The first placement appears in place; only moves between tabs travel.
    if (markerTop < 0) indicator.style.transition = "none";
    indicator.style.opacity = "1";
    indicator.style.transform = `translateY(${top}px)`;
    if (markerTop < 0) {
      void indicator.offsetWidth;
      indicator.style.removeProperty("transition");
    }
    if (markerTop >= 0 && Math.abs(top - markerTop) > 4) {
      animate(indicator.firstElementChild ?? indicator, [{ transform: "scaleY(1)" }, { transform: `scaleY(${Math.min(2.6, 1 + Math.abs(top - markerTop) / 70).toFixed(2)})`, offset: 0.35 }, { transform: "scaleY(1)" }], { duration: 460, easing: "cubic-bezier(.3,.7,.3,1)" });
    }
    markerTop = top;
  };
  indicator.appendChild(h("span", { class: "tab-indicator-drop" }));

  let lastChrome: Chrome = "none";
  let lastTab: TabId | undefined;
  attachShell({
    outlet,
    setChrome(chrome, tab) {
      lastChrome = chrome;
      lastTab = tab;
      document.documentElement.dataset.chrome = chrome;
      tabs.hidden = chrome !== "full";
      steps.hidden = chrome !== "setup";
      requestAnimationFrame(() => moveIndicator(tab));
    },
    announce(text) {
      announcer.textContent = "";
      setTimeout(() => (announcer.textContent = text), 30);
    },
    loading(on) {
      loadingBar.classList.toggle("is-on", on);
    },
  });
  window.addEventListener("resize", () => {
    if (lastChrome === "full") moveIndicator(lastTab);
  });

  // Glyph hints only make sense while a pad is the active input.
  const syncHints = () => {
    const pad = document.documentElement.dataset.input === "pad" && !!activeKind.value;
    document.documentElement.toggleAttribute("data-pad-hints", pad);
  };
  onInputMode(syncHints);
  activeKind.subscribe(syncHints);

  return {
    outlet,
    setVersions(launcherVersion, emulatorVersion) {
      const parts = [`Launcher ${launcherVersion ? v(launcherVersion) : "–"}`];
      if (emulatorVersion) parts.push(`Guarana ${v(emulatorVersion)}`);
      versions.textContent = parts.join(" · ");
    },
  };
}

/** Keeps the footer's emulator version in sync with get_state. */
export async function refreshVersions(shell: Shell): Promise<void> {
  const [appVersion, state] = await Promise.all([
    api.appVersion().catch(() => null),
    launcher.get().catch(() => null),
  ]);
  shell.setVersions(appVersion, state?.current ?? null);
}

export function hideBoot(): void {
  const boot = document.getElementById("boot");
  if (!boot) return;
  boot.classList.add("is-done");
  const remove = () => boot.remove();
  boot.addEventListener("transitionend", remove, { once: true });
  setTimeout(remove, 900);
}

export function tabButtons(): HTMLButtonElement[] {
  return $$<HTMLButtonElement>(".tab");
}
