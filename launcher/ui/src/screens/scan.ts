// Scan: detect_system, locate_files {scope:"quick"} and run_doctor run together and stream into a
// live checklist; result cards fill in as each finishes. Nothing is imported until you press Import.

import { api, errorText, events } from "../api";
import { fill, h } from "../dom";
import { basename, dirname, gpuName, plural, prettyPath } from "../format";
import { icon, type IconName } from "../icons";
import { settleFocus } from "../input";
import { go, preload, type ScreenDef } from "../router";
import { checks, controllers, found, launcher, library, profileDraft, profiles, recommendedProfile, systemInfo } from "../state";
import type { Check, Found, LauncherState, Profile, Progress, SystemInfo } from "../types";
import { checkRow, sortChecks, summarize } from "../ui/checks";
import { badge, busy, button, card, skeleton } from "../ui/controls";
import { controllersCard } from "../ui/controllers-card";
import { toast } from "../ui/toast";

type StepState = "running" | "done" | "warn" | "fail";

interface Step {
  el: HTMLElement;
  set(state: StepState, detail: string): void;
}

function step(i: number, title: string, detail: string): Step {
  const iconSlot = h("span", { class: "step-icon" }, h("span", { class: "spinner" }));
  const detailEl = h("p", { class: "step-detail", text: detail });
  const el = h("li", { class: "step", "data-state": "running", style: { "--i": String(i) } }, iconSlot, h("div", { class: "step-text" }, h("p", { class: "step-title", text: title }), detailEl));
  const icons: Record<StepState, IconName | null> = { running: null, done: "ok", warn: "warn", fail: "fail" };
  return {
    el,
    set(state, text) {
      el.dataset.state = state;
      detailEl.textContent = text;
      const name = icons[state];
      iconSlot.replaceChildren(name ? icon(name) : h("span", { class: "spinner" }));
    },
  };
}

function isSetup(): boolean {
  return launcher.peek()?.first_run !== false;
}

const screen: ScreenDef = {
  title: "Scan",
  chrome: () => (isSetup() ? "setup" : "full"),
  step: 1,

  render(ctx) {
    preload("home");
    preload("profile");
    const setup = isSetup();
    const deep = ctx.params.deep === true;

    const steps = {
      device: step(0, "Your device", "Reading the GPU, drivers and display…"),
      files: step(1, "Your files", deep ? "Searching all drives…" : "Looking in the usual places…"),
      health: step(2, "Health", "Running checks…"),
      pads: step(3, "Controllers", "Looking for controllers…"),
    };

    const deviceSlot = h("div", { class: "slot slot-device" }, card("Your device", { icon: "laptop", kicker: "Detecting" }, skeleton([62, 88, 74, 54, 66])));
    const filesSlot = h("div", { class: "slot slot-files" }, card("Your files", { icon: "folder", kicker: "Searching" }, skeleton([80, 56, 70])));
    const healthSlot = h("div", { class: "slot slot-health" }, card("Health", { icon: "pulse", kicker: "Checking" }, skeleton([70, 84, 60])));

    const cont = button(setup ? "Continue" : "Back to Home", {
      kind: "primary",
      size: "lg",
      icon: setup ? undefined : "home",
      primary: true,
      attrs: { disabled: true },
      // First run: the Auto mode picks the settings from this PC, so there is no profile step.
      onClick: (_e, self) =>
        void (setup
          ? busy(self, () => api.setMode("auto"), "Setting up…")
              .then(() => launcher.get(true))
              .then(() => go("home", { reset: true, transition: "forward" }))
              .catch((e: unknown) => toast(`Couldn't finish setup: ${errorText(e)}`, { tone: "fail" }))
          : go("home", { transition: "back", reset: true })),
    });
    if (setup) cont.appendChild(icon("arrowRight"));

    const el = h(
      "div",
      { class: "scan" },
      h(
        "header",
        { class: "screen-head" },
        h("p", { class: "eyebrow", text: setup ? "Step 1 of 2" : "Scan" }),
        h("h1", { text: setup ? "Getting to know your PC" : "Your PC and your files" }),
        h("p", { class: "lede", text: "A few seconds. Nothing leaves your PC, and nothing is imported until you say so." }),
      ),
      h(
        "div",
        { class: "scan-layout" },
        h("ol", { class: "steps", "aria-label": "Progress" }, Object.values(steps).map((s) => s.el)),
        h("div", { class: "scan-cards" }, deviceSlot, h("div", { class: "slot slot-pads" }, controllersCard(ctx.signal)), filesSlot, healthSlot),
      ),
      h("div", { class: "actionbar" }, h("p", { class: "fine", text: "You can change all of this later." }), cont),
    );

    let pending = 3;
    const settle = () => {
      pending -= 1;
      if (pending > 0) return;
      cont.disabled = false;
      settleFocus(el);
    };

    // ---- Device and profile
    void Promise.all([systemInfo.get(), profiles.get().catch(() => [] as Profile[])])
      .then(([sys, profs]) => {
        if (ctx.signal.aborted) return;
        const gpu = sys.gpus[sys.primary_gpu] ?? sys.gpus[0];
        steps.device.set("done", `${sys.device.manufacturer} ${shortModel(sys.device.model)} · ${gpu ? gpuName(gpu.name) : "GPU unknown"}`);
        reveal(deviceSlot, deviceCard(sys, profs));
      })
      .catch((e: unknown) => {
        steps.device.set("fail", errorText(e));
        reveal(deviceSlot, card("Your device", { icon: "laptop" }, h("p", { class: "fine", text: "Couldn't read this PC's hardware. You can still pick a profile yourself on the next step." })));
      })
      .finally(settle);

    // ---- Files (listen before locating, so no progress event is missed)
    // locate://progress counts search roots (drives, known folders); message is the one being searched.
    const locateListening = events.locateProgress((p: Progress) => {
      if (p.stage === "done") return;
      const where = p.message ? `Looking in ${prettyPath(p.message)}` : "Looking…";
      steps.files.set("running", p.total > 1 ? `${where} · ${Math.min(p.done + 1, p.total)} of ${p.total}` : where);
    }, ctx.signal);
    const runLocate = async (scope: "quick" | "deep") => {
      steps.files.set("running", scope === "deep" ? "Searching all drives…" : "Looking in the usual places…");
      await locateListening;
      // No roots: a deep search covers every fixed drive.
      const result = await api.locateFiles(scope);
      const state = await launcher.get(true);
      found.set(result);
      return { result, state };
    };
    const showFiles = (result: Found, state: LauncherState, scope: "quick" | "deep") => {
      const keys = state.keys_present || result.keys.some((k) => k.kind === "prod");
      const summary = [
        result.games.length ? plural(result.games.length, "game", "games") : "no games",
        result.firmware.length || state.firmware_present ? "firmware" : "no firmware",
        keys ? "keys" : "no keys yet",
      ].join(", ");
      steps.files.set(keys && (result.games.length || state.game_folders.length) ? "done" : "warn", `Found ${summary}`);
      reveal(filesSlot, filesCard(result, state, scope, async () => {
        const next = await runLocate("deep");
        showFiles(next.result, next.state, "deep");
      }));
    };
    void runLocate(deep ? "deep" : "quick")
      .then(({ result, state }) => {
        if (!ctx.signal.aborted) showFiles(result, state, deep ? "deep" : "quick");
      })
      .catch((e: unknown) => {
        steps.files.set("fail", errorText(e));
        reveal(filesSlot, card("Your files", { icon: "folder" }, h("p", { class: "fine", text: `The search stopped: ${errorText(e)}` })));
      })
      .finally(settle);

    // ---- Health
    void (async () => {
      await events.doctorProgress((p: Progress) => {
        if (p.stage === "done") return;
        steps.health.set("running", `${p.message ?? "Checking"}… ${Math.min(p.done + 1, p.total)} of ${p.total}`);
      }, ctx.signal);
      return api.runDoctor();
    })()
      .then((list) => {
        if (ctx.signal.aborted) return;
        checks.set(list);
        const s = summarize(list);
        const toFix = s.fail + s.warn;
        steps.health.set(s.fail ? "fail" : s.warn ? "warn" : "done", toFix ? `${plural(toFix, "thing", "things")} to look at, ${s.ok} good` : "All good");
        reveal(healthSlot, healthCard(list));
      })
      .catch((e: unknown) => {
        steps.health.set("fail", errorText(e));
        reveal(healthSlot, card("Health", { icon: "pulse" }, h("p", { class: "fine", text: `Checks didn't finish: ${errorText(e)}` })));
      })
      .finally(settle);

    // ---- Controllers (the card itself is live; the step mirrors it)
    const padStep = () => {
      const list = controllers.value.filter((c) => c.connected);
      if (list.length) steps.pads.set("done", `${list[0].name}${list.length > 1 ? ` +${list.length - 1}` : ""}`);
      else steps.pads.set("done", "None yet. Keyboard and mouse work too.");
    };
    const padTimer = setTimeout(padStep, 900);
    controllers.subscribe(padStep, ctx.signal);
    ctx.onLeave(() => clearTimeout(padTimer));

    return el;
  },
};

export default screen;

function reveal(slot: HTMLElement, content: HTMLElement): void {
  content.classList.add("card-reveal");
  slot.replaceChildren(content);
}

function shortModel(model: string): string {
  return model.replace(/\s*\([^)]*\)\s*$/, "");
}

function deviceCard(sys: SystemInfo, profs: Profile[]): HTMLElement {
  const gpu = sys.gpus[sys.primary_gpu] ?? sys.gpus[0];
  const formIcon: IconName = sys.device.form === "desktop" ? "display" : sys.device.form === "handheld" ? "gamepad" : "laptop";
  const spec = (label: string, value: string, iconName: IconName) =>
    h("div", { class: "spec" }, h("dt", null, icon(iconName), label), h("dd", { text: value }));
  const display = `${sys.display.width} × ${sys.display.height} · ${sys.display.refresh_hz} Hz${sys.display.vrr ? " · VRR" : ""}`;
  const power =
    sys.power.on_ac === null ? "Unknown" : sys.power.on_ac ? `Plugged in${sys.power.plan ? ` · ${sys.power.plan}` : ""}` : `On battery${sys.power.battery_percent !== null ? ` · ${sys.power.battery_percent}%` : ""}`;

  const rec = recommendedProfile(profs, sys);
  if (!profileDraft.id && rec) profileDraft.id = rec.id;
  const pickedName = h("strong", { class: "pick-name" });
  const pickedBadge = h("span", { class: "pick-badge" });
  const list = h("div", { class: "pick-list", role: "group", "aria-label": "Device profile", hidden: true });
  const change = h("button", { type: "button", class: "link-btn", "aria-expanded": "false" }, "Change", icon("chevronDown"));
  const sync = () => {
    const p = profs.find((x) => x.id === profileDraft.id) ?? rec;
    pickedName.textContent = p ? p.name : "Safe defaults";
    fill(pickedBadge, p?.recommended ? badge("Recommended", "ok", "leaf") : badge("Your pick", "accent"));
    for (const b of Array.from(list.querySelectorAll<HTMLButtonElement>(".pick"))) b.setAttribute("aria-pressed", String(b.dataset.id === profileDraft.id));
  };
  for (const p of profs) {
    list.appendChild(
      h(
        "button",
        {
          type: "button",
          class: "pick",
          "data-id": p.id,
          "aria-pressed": "false",
          onClick: () => {
            profileDraft.id = p.id;
            profileDraft.overrides = {};
            sync();
          },
        },
        h("span", { class: "pick-title", text: p.name }),
        p.recommended ? h("span", { class: "pick-rec", text: "Recommended" }) : null,
      ),
    );
  }
  change.addEventListener("click", () => {
    const open = list.hidden;
    list.hidden = !open;
    change.setAttribute("aria-expanded", String(open));
    change.classList.toggle("is-open", open);
  });
  sync();

  return card(
    "Your device",
    { icon: formIcon, kicker: "Detected" },
    h("p", { class: "device-name", text: `${sys.device.manufacturer} ${sys.device.model}` }),
    h(
      "dl",
      { class: "specs" },
      spec("GPU", gpu ? `${gpuName(gpu.name)} · driver ${gpu.driver_version}` : "Not found", "chip"),
      spec("CPU", `${sys.cpu.name} · ${sys.cpu.threads} threads`, "code"),
      spec("Memory", `${sys.ram_gb} GB RAM`, "memory"),
      spec("Display", display, "display"),
      spec("Power", power, "plug"),
    ),
    profs.length
      ? h("div", { class: "pick-row" }, h("span", { class: "pick-label", text: "Profile" }), pickedName, pickedBadge, h("span", { class: "grow" }), change)
      : null,
    profs.length ? list : null,
  );
}

function filesCard(result: Found, state: LauncherState, scope: "quick" | "deep", deepSearch: () => Promise<void>): HTMLElement {
  const item = (iconName: IconName, title: string, sub: string, action: HTMLElement | null, cls = "") =>
    h("li", { class: `item ${cls}` }, h("span", { class: "item-icon" }, icon(iconName)), h("div", { class: "grow" }, h("p", { class: "item-title", text: title }), h("p", { class: "item-sub", text: sub })), action ? h("div", { class: "item-actions" }, action) : null);

  // Keys: one Import per file. Never automatic.
  const keyItems = state.keys_present
    ? [item("key", "Keys", "Already in Guarana's keys folder", badge("Imported", "ok", "ok"), "is-done")]
    : result.keys.length
      ? result.keys.map((k) => {
          const li = item("key", basename(k.path), `${k.kind === "prod" ? "Needed" : "Optional"} · ${prettyPath(dirname(k.path))}`, null);
          const btn = button("Import", {
            kind: k.kind === "prod" ? "primary" : "secondary",
            size: "sm",
            onClick: (_e, self) =>
              void busy(self, () => api.importKeys(k.path), "Importing…")
                .then((r) => {
                  if (!r.ok) throw new Error(r.message);
                  li.classList.add("is-done");
                  self.replaceWith(badge("Imported", "ok", "ok"));
                  toast(r.message || `Imported ${basename(k.path)}.`, { tone: "ok" });
                  void launcher.get(true);
                })
                .catch((e: unknown) => toast(`${basename(k.path)}: ${errorText(e)}`, { tone: "fail" })),
          });
          li.appendChild(h("div", { class: "item-actions" }, btn));
          return li;
        })
      : [item("key", "No keys found", "Dump prod.keys from your Switch 2, then search again.", null, "is-missing")];

  // Firmware: shown where it was found. The contract has no firmware import command yet.
  const fwItems = state.firmware_present
    ? [item("chip", "Firmware", "Installed", badge("Installed", "ok", "ok"), "is-done")]
    : result.firmware.length
      ? result.firmware.map((f) =>
          item(
            "chip",
            f.version ? `Firmware ${f.version}` : "Firmware",
            prettyPath(f.path),
            button("Show folder", { kind: "ghost", size: "sm", icon: "folder", onClick: () => void api.openPath(f.path).catch((e: unknown) => toast(errorText(e), { tone: "fail" })) }),
          ),
        )
      : [item("chip", "No firmware found", "Dump it from your console too.", null, "is-missing")];

  // Games: one Add per folder.
  const byFolder = new Map<string, typeof result.games>();
  for (const g of result.games) {
    const dir = dirname(g.path);
    byFolder.set(dir, [...(byFolder.get(dir) ?? []), g]);
  }
  const gameItems = byFolder.size
    ? Array.from(byFolder.entries()).map(([dir, games]) => {
        const oot = games.find((g) => g.is_ocarina);
        const lead = oot ? "Ocarina of Time" : games[0].title;
        const sub = games.length > 1 ? `${lead} + ${games.length - 1} more · ${prettyPath(dir)}` : `${lead} · ${prettyPath(dir)}`;
        const added = state.game_folders.some((f) => f.toLowerCase() === dir.toLowerCase());
        if (added) return item("folder", plural(games.length, "game", "games"), sub, badge("In library", "ok", "ok"), "is-done");
        const li = item("folder", plural(games.length, "game", "games"), sub, null);
        const btn = button("Add folder", {
          kind: "primary",
          size: "sm",
          onClick: (_e, self) =>
            void busy(self, () => api.addGameFolder(dir), "Adding…")
              .then((lib) => {
                library.set(lib);
                li.classList.add("is-done");
                self.replaceWith(badge("In library", "ok", "ok"));
                toast(`Added ${plural(games.length, "game", "games")} from ${prettyPath(dir)}.`, { tone: "ok" });
                void launcher.get(true);
              })
              .catch((e: unknown) => toast(errorText(e), { tone: "fail" })),
        });
        li.appendChild(h("div", { class: "item-actions" }, btn));
        return li;
      })
    : [item("game", "No games found", scope === "deep" ? "Nothing on any drive. Games must be dumped from cartridges or purchases you own." : "Try searching all drives.", null, "is-missing")];

  const deepBtn = button(scope === "deep" ? "Search again" : "Search all drives", {
    kind: "secondary",
    size: "sm",
    icon: "search",
    onClick: (_e, self) => void busy(self, deepSearch, "Searching…").catch((e: unknown) => toast(errorText(e), { tone: "fail" })),
  });

  const pickBtn = button("Add folder", {
    kind: "secondary",
    size: "sm",
    icon: "folder",
    onClick: (_e, self) =>
      void busy(
        self,
        async () => {
          const dir = await api.pickFolder("Choose a folder with your games");
          if (!dir) return null;
          return { dir, lib: await api.addGameFolder(dir) };
        },
        "Adding…",
      )
        .then((r) => {
          if (!r) return;
          library.set(r.lib);
          toast(`Added ${prettyPath(r.dir)}. ${plural(r.lib.length, "game", "games")} in your library.`, { tone: "ok" });
          void launcher.get(true);
        })
        .catch((e: unknown) => toast(errorText(e), { tone: "fail" })),
  });

  return card(
    "Your files",
    { icon: "folder", kicker: `${plural(result.scanned_dirs, "folder", "folders")} in ${result.seconds.toFixed(1)} s`, actions: h("div", { class: "row" }, pickBtn, deepBtn) },
    h(
      "div",
      { class: "files-grid" },
      h("div", { class: "file-group" }, h("p", { class: "group-label", text: "Keys" }), h("ul", { class: "items" }, keyItems)),
      h("div", { class: "file-group" }, h("p", { class: "group-label", text: "Firmware" }), h("ul", { class: "items" }, fwItems)),
      h("div", { class: "file-group" }, h("p", { class: "group-label", text: "Games" }), h("ul", { class: "items" }, gameItems)),
    ),
    h("p", { class: "legal" }, icon("shield"), "Guarana never downloads keys, firmware or games. Use your own, dumped from your console."),
  );
}

function healthCard(list: Check[]): HTMLElement {
  const s = summarize(list);
  const issues = sortChecks(list).filter((c) => c.status === "fail" || c.status === "warn");
  const shown = issues.slice(0, 4);
  return card(
    "Health",
    {
      icon: "pulse",
      kicker: `${plural(list.length, "check", "checks")}`,
      actions: h("button", { type: "button", class: "link-btn", onClick: () => void go("doctor") }, "See all", icon("chevronRight")),
    },
    h(
      "div",
      { class: "row health-summary" },
      s.fail ? badge(`${s.fail} to fix`, "fail", "fail") : null,
      s.warn ? badge(`${s.warn} to check`, "warn", "warn") : null,
      badge(`${s.ok} good`, "ok", "ok"),
    ),
    shown.length
      ? h("ul", { class: "checks" }, shown.map((c) => checkRow(c, { compact: true, onFixed: (u) => checks.set((checks.value ?? list).map((x) => (x.id === u.id ? u : x))) })))
      : h("p", { class: "fine", text: "Everything Guarana needs is in place." }),
    issues.length > shown.length ? h("p", { class: "fine", text: `${issues.length - shown.length} more in Doctor.` }) : null,
  );
}
