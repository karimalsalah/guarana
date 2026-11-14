// "DualSense detected - ready to play": the controller card on Scan and Home, with a face-button
// diamond that pulses on connect and lights up live while you press buttons.

import { fill, h } from "../dom";
import { browserPads, faceButton, glyph, LAYOUTS, onPadButton, padName } from "../gamepad";
import { icon } from "../icons";
import { reducedMotion } from "../motion";
import { controllers } from "../state";
import type { ControllerKind } from "../types";

interface PadView {
  name: string;
  kind: ControllerKind;
  battery: number | null;
  count: number;
}

function shortName(name: string, kind: ControllerKind): string {
  if (/dualsense edge/i.test(name)) return "DualSense Edge";
  if (/dualsense/i.test(name)) return "DualSense";
  if (/dualshock/i.test(name)) return "DualShock 4";
  if (kind === "xbox") return "Xbox controller";
  if (/pro controller/i.test(name)) return "Pro Controller";
  return name.replace(/\s*\(.*\)$/, "").trim() || "Controller";
}

function currentPad(): PadView | null {
  const list = controllers.value.filter((c) => c.connected);
  if (list.length) return { name: list[0].name, kind: list[0].kind, battery: list[0].battery, count: list.length };
  const pads = browserPads.value;
  if (pads.length) return { name: padName(pads[0].id), kind: pads[0].kind, battery: null, count: pads.length };
  return null;
}

export function controllersCard(signal: AbortSignal): HTMLElement {
  const title = h("h2", { class: "card-title", id: "pad-card-title" });
  const status = h("p", { class: "pad-status" });
  const hint = h("p", { class: "pad-hint" });
  const visual = h("div", { class: "pad-visual" });
  const el = h(
    "section",
    { class: "card pad-card", "aria-labelledby": "pad-card-title", "aria-live": "polite" },
    visual,
    h("div", { class: "pad-text" }, h("p", { class: "card-kicker", text: "Controller" }), title, status, hint),
  );

  let shownKind: ControllerKind | null = null;
  let firstRender = true;

  const render = () => {
    const pad = currentPad();
    if (!pad) {
      shownKind = null;
      el.classList.add("is-empty");
      visual.replaceChildren(h("span", { class: "pad-empty-icon" }, icon("gamepad")));
      visual.removeAttribute("data-kind");
      title.textContent = "No controller yet";
      status.textContent = "Plug one in or pair it over Bluetooth.";
      hint.textContent = "Keyboard and mouse work too.";
      firstRender = false;
      return;
    }
    el.classList.remove("is-empty");
    const layout = LAYOUTS[pad.kind];
    const isNew = shownKind !== pad.kind;
    if (isNew) {
      visual.dataset.kind = pad.kind;
      const face = h(
        "div",
        { class: "pad-face" },
        [3, 2, 1, 0].map((i) => {
          const b = faceButton(i, pad.kind);
          b.classList.add(`pos-${["bottom", "right", "left", "top"][i]}`);
          if (i === layout.confirm) b.classList.add("is-confirm");
          return b;
        }),
      );
      visual.replaceChildren(h("span", { class: "pad-ring", "aria-hidden": "true" }), face);
      if (!firstRender && !reducedMotion()) {
        visual.classList.remove("is-connecting");
        void visual.offsetWidth;
        visual.classList.add("is-connecting");
      } else if (firstRender) {
        visual.classList.add("is-connecting");
      }
      shownKind = pad.kind;
    }
    title.textContent = `${shortName(pad.name, pad.kind)} detected`;
    fill(
      status,
      h("span", { class: "pad-ready" }, icon("ok"), "Ready to play"),
      pad.battery !== null ? h("span", { class: `pad-battery${pad.battery < 20 ? " is-low" : ""}` }, batteryIcon(pad.battery), `${Math.round(pad.battery)}%`) : null,
      pad.count > 1 ? h("span", { class: "pad-more", text: `+${pad.count - 1} more` }) : null,
    );
    hint.replaceChildren("Press ", glyph("confirm"), " to select, ", glyph("back"), " to go back.");
    firstRender = false;
  };

  render();
  controllers.subscribe(render, signal);
  browserPads.subscribe(render, signal);
  onPadButton((e) => {
    if (e.index > 3) return;
    visual.querySelector(`.face-btn[data-index="${e.index}"]`)?.classList.toggle("is-down", e.pressed);
  }, signal);
  return el;
}

function batteryIcon(level: number): HTMLElement {
  const fill = h("span", { class: "battery-fill" });
  fill.style.width = `${Math.max(6, Math.min(100, level))}%`;
  return h("span", { class: "battery", "aria-hidden": "true" }, fill);
}
