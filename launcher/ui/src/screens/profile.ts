// Profile: device profiles as large toggle cards, then that profile's switches with one-line help.
// Changes stay a draft until Apply (apply_profile with only the values you changed).

import { api, errorText } from "../api";
import { h, nextId } from "../dom";
import { gpuName, plural } from "../format";
import { icon, type IconName } from "../icons";
import { settleFocus } from "../input";
import { go, type ScreenDef } from "../router";
import { launcher, profileDraft, profiles, recommendedProfile, systemInfo } from "../state";
import type { Overrides, Profile, SystemInfo, Toggle, ToggleValue } from "../types";
import { badge, busy, button, segmented, skeleton, stepper, switchControl } from "../ui/controls";
import { toast } from "../ui/toast";

const PROFILE_ICON: Record<string, IconName> = {
  "intel-arc-igpu": "chip",
  "amd-igpu-handheld": "gamepad",
  "low-power-handheld": "leaf",
  "desktop-dgpu": "display",
  "safe-defaults": "shield",
};

function isSetup(): boolean {
  return launcher.peek()?.first_run !== false;
}

const screen: ScreenDef = {
  title: "Profile",
  chrome: () => (isSetup() ? "setup" : "full"),
  tab: "profile",

  render(ctx) {
    const setup = isSetup();
    const lede = h("p", { class: "lede", text: "Pick the closest match. You can change anything, any time." });
    const cardsSlot = h("div", { class: "profile-cards", role: "group", "aria-label": "Device profile" }, [0, 1, 2].map(() => h("div", { class: "profile-card is-skeleton" }, skeleton([50, 85, 70]))));
    const togglesTitle = h("h2", { class: "card-title", id: "toggles-title", text: "Settings" });
    const reset = h("button", { type: "button", class: "link-btn", hidden: true }, icon("refresh"), "Reset to profile defaults");
    const togglesList = h("ul", { class: "toggle-list" }, skeleton([90, 75, 85, 60], "skeleton-tall"));
    const toggles = h("section", { class: "card toggles", "aria-labelledby": "toggles-title" }, h("header", { class: "card-head" }, h("div", { class: "card-titles" }, h("p", { class: "card-kicker", text: "In this profile" }), togglesTitle), reset), togglesList);
    const note = h("p", { class: "fine", text: "Applying writes nx2_switches.txt and qt-config.ini in Guarana's own folder, and Home shows Custom until you pick a mode again." });
    const apply = button(setup ? "Apply and continue" : "Apply", { kind: "primary", size: "lg", primary: true, attrs: { disabled: true } });
    if (setup) apply.appendChild(icon("arrowRight"));

    const el = h(
      "div",
      { class: "profile" },
      h("header", { class: "screen-head" }, h("p", { class: "eyebrow", text: setup ? "Step 2 of 2" : "Profile" }), h("h1", { text: "How should Guarana run?" }), lede),
      cardsSlot,
      toggles,
      h("div", { class: "actionbar" }, note, apply),
    );

    // list_profiles returns the applied profile with the user's saved values already merged in
    // (`value`), next to the profile's own (`default`). apply_profile replaces the saved overrides,
    // so Apply sends the toggles that differ from `default`, and "Restore defaults" sends none.
    let profileList: Profile[] = [];
    let appliedId: string | null = null;
    let applied = false;
    const currentProfile = () => profileList.find((p) => p.id === profileDraft.id);

    void Promise.all([profiles.get(true), systemInfo.get().catch(() => undefined), launcher.get().catch(() => undefined)])
      .then(([list, sys, state]) => {
        if (ctx.signal.aborted) return;
        if (!list.length) throw new Error("No profiles came back from the launcher.");
        profileList = list;
        appliedId = state?.profile_id ?? null;
        const rec = recommendedProfile(list, sys);
        if (!profileDraft.id || !list.some((p) => p.id === profileDraft.id)) profileDraft.id = state?.profile_id ?? rec?.id ?? list[0].id;
        lede.textContent = rec && sys ? recommendation(rec, sys) : lede.textContent;
        build(list);
        settleFocus(el);
      })
      .catch((e: unknown) => {
        cardsSlot.replaceChildren(h("p", { class: "fine", text: `Profiles didn't load: ${errorText(e)}` }));
        togglesList.replaceChildren();
      });

    const markDirty = () => {
      applied = false;
      apply.disabled = false;
      const label = apply.querySelector(".btn-label");
      if (label) label.textContent = setup ? "Apply and continue" : "Apply";
    };

    const syncReset = (p: Profile) => {
      const edited = Object.keys(profileDraft.overrides).length > 0;
      reset.hidden = !edited && p.id !== appliedId;
      reset.replaceChildren(icon(edited ? "history" : "refresh"), edited ? "Undo changes" : "Restore defaults");
    };

    reset.addEventListener("click", () => {
      const p = currentProfile();
      if (!p) return;
      if (Object.keys(profileDraft.overrides).length) {
        profileDraft.overrides = {};
        renderToggles(p, true);
        return;
      }
      reset.disabled = true;
      api
        .applyProfile(p.id)
        .then(() => profiles.get(true))
        .then((list) => {
          profileList = list;
          appliedId = p.id;
          const fresh = list.find((x) => x.id === p.id);
          if (fresh) renderToggles(fresh, true);
          void launcher.get(true);
          toast(`Back to the ${p.name} defaults.`, { tone: "ok" });
        })
        .catch((e: unknown) => toast(`Couldn't restore the defaults: ${errorText(e)}`, { tone: "fail" }))
        .finally(() => {
          reset.disabled = false;
        });
    });

    apply.addEventListener("click", () => {
      const p = currentProfile();
      if (!p || applied) return;
      // Only what differs from the profile's own value (toggle.default) goes to the backend.
      const values: Overrides = {};
      for (const t of p.toggles) {
        const val = t.id in profileDraft.overrides ? profileDraft.overrides[t.id] : t.value;
        if (val !== t.default) values[t.id] = val;
      }
      void busy(apply, () => api.applyProfile(p.id, values), "Applying…")
        .then((r) => {
          applied = true;
          appliedId = p.id;
          profileDraft.overrides = {};
          void launcher.get(true);
          void profiles.get(true).then((list) => {
            profileList = list;
            const fresh = list.find((x) => x.id === p.id);
            if (fresh && !setup && !ctx.signal.aborted) renderToggles(fresh, false);
          });
          toast(`${p.name} applied. ${plural(r.changed.length, "setting", "settings")} written.`, { tone: "ok" });
          if (setup) {
            void go("home", { reset: true, transition: "forward" });
          } else {
            apply.disabled = true;
            const label = apply.querySelector(".btn-label");
            if (label) label.textContent = "Applied";
          }
        })
        .catch((e: unknown) => toast(`Couldn't apply ${p.name}: ${errorText(e)}`, { tone: "fail" }));
    });

    function build(list: Profile[]) {
      const cards: HTMLButtonElement[] = list.map((p) => {
        const b = h(
          "button",
          { type: "button", class: "profile-card", "aria-pressed": String(p.id === profileDraft.id), "data-id": p.id, "data-autofocus": p.id === profileDraft.id ? true : undefined },
          h("span", { class: "profile-icon" }, icon(PROFILE_ICON[p.id] ?? "sliders")),
          h("span", { class: "profile-name", text: p.name }),
          h("span", { class: "profile-desc", text: p.description }),
          h("span", { class: "profile-badges" }, p.recommended ? badge("Recommended", "ok", "leaf") : null),
          h("span", { class: "profile-check", "aria-hidden": "true" }, icon("check")),
        );
        b.addEventListener("click", () => {
          if (profileDraft.id === p.id) return;
          profileDraft.id = p.id;
          profileDraft.overrides = {};
          for (const c of cards) c.setAttribute("aria-pressed", String(c.dataset.id === p.id));
          renderToggles(p, true);
          markDirty();
        });
        return b;
      });
      cardsSlot.replaceChildren(...cards);
      const current = list.find((p) => p.id === profileDraft.id) ?? list[0];
      renderToggles(current, false);
      apply.disabled = false;
    }

    function renderToggles(p: Profile, animate: boolean) {
      togglesTitle.textContent = `Settings in ${p.name}`;
      syncReset(p);
      togglesList.replaceChildren(
        ...p.toggles.map((t, i) => {
          const row = toggleRow(t, (value) => {
            if (value === t.value) delete profileDraft.overrides[t.id];
            else profileDraft.overrides[t.id] = value;
            syncReset(p);
            row.classList.toggle("is-changed", t.id in profileDraft.overrides);
            markDirty();
          });
          if (t.id in profileDraft.overrides) row.classList.add("is-changed");
          if (animate) {
            row.classList.add("check-reveal");
            row.style.setProperty("--i", String(i));
          }
          return row;
        }),
      );
    }

    return el;
  },
};

export default screen;

function recommendation(rec: Profile, sys: SystemInfo): string {
  const gpu = sys.gpus[sys.primary_gpu] ?? sys.gpus[0];
  return gpu ? `We picked ${rec.name} for your ${gpuName(gpu.name)}. You can change anything, any time.` : `We picked ${rec.name}. You can change anything, any time.`;
}

/** "0.75x" -> "0.75×", "off" -> "Off", "conservative" -> "Conservative". The value sent stays as is. */
function choiceLabel(c: string): string {
  if (/^\d+(\.\d+)?x$/i.test(c)) return `${c.slice(0, -1)}×`;
  return /^[a-z]/.test(c) ? c[0].toUpperCase() + c.slice(1) : c;
}

function toggleRow(t: Toggle, onChange: (v: ToggleValue) => void): HTMLLIElement {
  const labelId = nextId("t");
  const helpId = nextId("h");
  const current: ToggleValue = t.id in profileDraft.overrides ? profileDraft.overrides[t.id] : t.value;
  let control: HTMLElement;
  if (t.kind === "bool") {
    control = switchControl({ checked: current === true, labelledBy: labelId, describedBy: helpId, onChange });
  } else if (t.kind === "choice" && t.choices && t.choices.length <= 6 && t.choices.every((c) => c.length <= 12)) {
    // Short choices stay one press away for pad users: segmented buttons, not a dropdown.
    control = segmented({ choices: t.choices.map((c) => ({ value: c, label: choiceLabel(c) })), value: String(current), label: t.label, size: "sm", onChange }).el;
  } else if (t.kind === "choice" && t.choices) {
    const select = h("select", { class: "input select", "aria-labelledby": labelId, "aria-describedby": helpId }, t.choices.map((c) => h("option", { value: c, selected: c === String(current) ? true : undefined }, choiceLabel(c))));
    select.addEventListener("change", () => onChange(select.value));
    control = select;
  } else if (t.min !== undefined && t.max !== undefined) {
    control = stepper({ value: Number(current), min: t.min, max: t.max, step: t.step, unit: t.unit, label: t.label, onChange });
  } else {
    const input = h("input", { class: "input input-num", type: "number", value: String(current), "aria-labelledby": labelId, "aria-describedby": helpId });
    input.addEventListener("change", () => {
      const n = Number(input.value);
      if (Number.isFinite(n)) onChange(n);
    });
    control = input;
  }
  return h(
    "li",
    { class: "toggle-row" },
    h(
      "div",
      { class: "toggle-text" },
      h("p", { class: "toggle-label", id: labelId }, t.label, t.experimental ? badge("Experimental", "info", "flask") : null),
      h("p", { class: "toggle-help", id: helpId, text: t.help }),
    ),
    h("div", { class: "toggle-control" }, control),
  );
}
