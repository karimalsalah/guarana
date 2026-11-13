// App chrome: top bar (brand, tabs, theme), the outlet, the footer and the live regions.

import { AI_QUICKSTART_URL, DISCORD_URL } from "./ui/ask-ai";
import { api, inTauri } from "./api";
import { $$, h } from "./dom";
import { activeKind, glyph } from "./gamepad";
import { icon, logoMark } from "./icons";
import { onInputMode } from "./input";
import { attachShell, go, goTab, TABS, type Chrome, type TabId } from "./router";
import { launcher } from "./state";
import { currentTheme, onThemeChange, setTheme } from "./theme";
import { v } from "./format";

const GITHUB = "https://github.com/karimalsalah/guarana";
const DISCUSSIONS = "https://github.com/karimalsalah/guarana/discussions";

export interface Shell {
  outlet: HTMLElement;
  setVersions(launcherVersion: string | null, emulatorVersion: string | null): void;
}

export function mountShell(app: HTMLElement, onReport: () => void): Shell {
  // ---- Top bar
  const tabButtons = new Map<TabId, HTMLButtonElement>();
  const indicator = h("span", { class: "tab-indicator", "aria-hidden": "true" });
  const tabList = h(
    "div",
    { class: "tabs-list" },
    TABS.map((t) => {
      const b = h(
        "button",
        { class: "tab", type: "button", "data-tab": t.id, onClick: () => goTab(t.id) },
        h("span", { class: "tab-label", text: t.label }),
      );
      tabButtons.set(t.id, b);
      return b;
    }),
    indicator,
  );
  const tabs = h("nav", { class: "tabs", "aria-label": "Main" }, h("span", { class: "tab-hint" }, glyph("tabPrev")), tabList, h("span", { class: "tab-hint" }, glyph("tabNext")));

  const stepLabel = h("span", { class: "sr-only" });
  const steps = h("div", { class: "setup-steps", hidden: true }, h("span", { class: "steps-kicker", text: "Set up" }), stepLabel,
    h("span", { class: "steps-dots", "aria-hidden": "true" }, h("i"), h("i")));

  const themeBtn = h("button", { class: "icon-btn theme-toggle", type: "button" });
  const syncThemeBtn = () => {
    const light = currentTheme() === "light";
    themeBtn.replaceChildren(icon(light ? "moon" : "sun"));
    themeBtn.setAttribute("aria-label", light ? "Switch to the dark theme" : "Switch to the light theme");
    themeBtn.title = light ? "Dark theme" : "Light theme";
  };
  syncThemeBtn();
  onThemeChange(syncThemeBtn);
  themeBtn.addEventListener("click", () => {
    const r = themeBtn.getBoundingClientRect();
    void setTheme(currentTheme() === "light" ? "dark" : "light", { x: r.left + r.width / 2, y: r.top + r.height / 2 });
  });

  const topbar = h(
    "header",
    { class: "topbar" },
    h("div", { class: "brand" }, logoMark("brand-mark"), h("span", { class: "brand-name", text: "Guarana" })),
    tabs,
    steps,
    h("div", { class: "topbar-end" }, themeBtn),
  );

  // ---- Main
  const outlet = h("div", { class: "outlet" });
  const loadingBar = h("div", { class: "loading-bar", "aria-hidden": "true" });
  const main = h("main", { id: "main", tabindex: "-1" }, outlet);

  // ---- Footer
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
    { class: "bottombar" },
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

  app.replaceChildren(topbar, loadingBar, main, footer, toasts, announcer);

  // ---- Router hooks
  const moveIndicator = (tab: TabId | undefined) => {
    for (const [id, b] of tabButtons) {
      if (id === tab) b.setAttribute("aria-current", "page");
      else b.removeAttribute("aria-current");
    }
    const b = tab ? tabButtons.get(tab) : undefined;
    if (!b) {
      indicator.style.opacity = "0";
      return;
    }
    const left = b.offsetLeft + 12;
    const width = Math.max(8, b.offsetWidth - 24);
    indicator.style.opacity = "1";
    indicator.style.transform = `translateX(${left}px) scaleX(${width / 100})`;
  };

  let lastChrome: Chrome = "none";
  let lastTab: TabId | undefined;
  attachShell({
    outlet,
    setChrome(chrome, tab, step) {
      lastChrome = chrome;
      lastTab = tab;
      document.documentElement.dataset.chrome = chrome;
      tabs.hidden = chrome !== "full";
      steps.hidden = chrome !== "setup";
      if (chrome === "setup") {
        stepLabel.textContent = step ? `Step ${step} of 2` : "";
        steps.dataset.step = String(step ?? 1);
      }
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
