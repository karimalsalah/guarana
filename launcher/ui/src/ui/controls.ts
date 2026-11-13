// Shared controls: buttons, switches, segmented choices, steppers, badges, skeletons.

import { h, nextId, type Child } from "../dom";
import { clamp } from "../format";
import { icon, type IconName } from "../icons";
import type { CheckStatus } from "../types";

export function button(
  label: Child,
  opts: {
    kind?: "primary" | "secondary" | "ghost" | "play";
    size?: "sm" | "md" | "lg" | "xl";
    icon?: IconName;
    /** Gets the button too, so handlers never need to reference the variable they initialise. */
    onClick?: (e: MouseEvent, btn: HTMLButtonElement) => void;
    primary?: boolean;
    autofocus?: boolean;
    attrs?: Record<string, unknown>;
  } = {},
): HTMLButtonElement {
  const kind = opts.kind ?? "secondary";
  const size = opts.size ?? "md";
  const b = h(
    "button",
    {
      type: "button",
      class: `btn btn-${kind} btn-${size}`,
      "data-primary": opts.primary ? true : undefined,
      "data-autofocus": opts.autofocus ? true : undefined,
      ...(opts.attrs ?? {}),
    },
    opts.icon ? icon(opts.icon) : null,
    h("span", { class: "btn-label" }, label),
  );
  const onClick = opts.onClick;
  if (onClick) b.addEventListener("click", (e) => onClick(e, b));
  return b;
}

/** Puts a button into a busy state (spinner, disabled) until the promise settles. */
export async function busy<T>(btn: HTMLButtonElement, work: () => Promise<T>, busyLabel?: string): Promise<T> {
  const label = btn.querySelector<HTMLElement>(".btn-label");
  const before = label?.textContent ?? "";
  btn.disabled = true;
  btn.classList.add("is-busy");
  btn.setAttribute("aria-busy", "true");
  if (label && busyLabel) label.textContent = busyLabel;
  try {
    return await work();
  } finally {
    btn.disabled = false;
    btn.classList.remove("is-busy");
    btn.removeAttribute("aria-busy");
    if (label && busyLabel) label.textContent = before;
  }
}

export function switchControl(opts: { checked: boolean; labelledBy?: string; describedBy?: string; label?: string; onChange: (v: boolean) => void }): HTMLButtonElement {
  const b = h(
    "button",
    {
      type: "button",
      role: "switch",
      class: "switch",
      "aria-checked": String(opts.checked),
      "aria-labelledby": opts.labelledBy,
      "aria-describedby": opts.describedBy,
      "aria-label": opts.labelledBy ? undefined : opts.label,
    },
    h("span", { class: "switch-thumb" }),
  );
  b.addEventListener("click", () => {
    const next = b.getAttribute("aria-checked") !== "true";
    b.setAttribute("aria-checked", String(next));
    opts.onChange(next);
  });
  return b;
}

export function setSwitch(b: HTMLButtonElement, v: boolean): void {
  b.setAttribute("aria-checked", String(v));
}

export interface Segmented {
  el: HTMLElement;
  /** Selects a value without calling onChange. */
  set(value: string): void;
}

export function segmented(opts: { choices: { value: string; label: string; hint?: string }[]; value: string; label: string; onChange: (v: string) => void; size?: "sm" | "md" }): Segmented {
  const group = h("div", { class: `segmented segmented-${opts.size ?? "md"}`, role: "group", "aria-label": opts.label });
  const thumb = h("span", { class: "segmented-thumb", "aria-hidden": "true" });
  group.appendChild(thumb);
  const buttons: { value: string; el: HTMLButtonElement }[] = [];
  const place = () => {
    const active = buttons.find((b) => b.el.getAttribute("aria-pressed") === "true");
    if (!active || !active.el.offsetWidth) {
      thumb.style.opacity = "0";
      return;
    }
    thumb.style.opacity = "1";
    thumb.style.transform = `translateX(${active.el.offsetLeft}px)`;
    thumb.style.width = `${active.el.offsetWidth}px`;
  };
  const select = (value: string) => {
    for (const b of buttons) b.el.setAttribute("aria-pressed", String(b.value === value));
    place();
  };
  for (const c of opts.choices) {
    const b = h("button", { type: "button", class: "segment", "aria-pressed": String(c.value === opts.value), title: c.hint }, c.label);
    b.addEventListener("click", () => {
      if (b.getAttribute("aria-pressed") === "true") return;
      select(c.value);
      opts.onChange(c.value);
    });
    buttons.push({ value: c.value, el: b });
    group.appendChild(b);
  }
  if (typeof ResizeObserver === "function") new ResizeObserver(place).observe(group);
  requestAnimationFrame(place);
  return { el: group, set: select };
}

export function stepper(opts: { value: number; min?: number; max?: number; step?: number; unit?: string; label: string; onChange: (v: number) => void }): HTMLElement {
  const step = opts.step ?? 1;
  const min = opts.min ?? Number.NEGATIVE_INFINITY;
  const max = opts.max ?? Number.POSITIVE_INFINITY;
  let value = opts.value;
  const out = h("output", { class: "stepper-value", "aria-live": "polite" });
  const dec = h("button", { type: "button", class: "icon-btn stepper-btn", "aria-label": `Lower ${opts.label}` }, icon("minus"));
  const inc = h("button", { type: "button", class: "icon-btn stepper-btn", "aria-label": `Raise ${opts.label}` }, icon("plus"));
  const render = () => {
    out.textContent = opts.unit ? `${value} ${opts.unit}` : String(value);
    dec.disabled = value - step < min;
    inc.disabled = value + step > max;
  };
  const set = (n: number) => {
    const next = clamp(Math.round(n / step) * step, min, max);
    if (next === value) return;
    value = next;
    render();
    opts.onChange(value);
  };
  dec.addEventListener("click", () => set(value - step));
  inc.addEventListener("click", () => set(value + step));
  render();
  return h("div", { class: "stepper", role: "group", "aria-label": opts.label }, dec, out, inc);
}

export function badge(text: string, tone: "ok" | "warn" | "fail" | "info" | "accent" | "neutral" = "neutral", iconName?: IconName): HTMLElement {
  return h("span", { class: `badge badge-${tone}` }, iconName ? icon(iconName) : null, text);
}

const STATUS_TEXT: Record<CheckStatus, string> = { ok: "Good", warn: "Check", fail: "Fix", info: "Info" };
const STATUS_ICON: Record<CheckStatus, IconName> = { ok: "ok", warn: "warn", fail: "fail", info: "info" };

/** Status is always icon + word, never colour alone. */
export function statusPill(status: CheckStatus): HTMLElement {
  return h("span", { class: `status status-${status}` }, icon(STATUS_ICON[status]), h("span", { text: STATUS_TEXT[status] }));
}

export function skeleton(lines: number[] = [70, 45], cls = ""): HTMLElement {
  return h("div", { class: `skeleton ${cls}`, "aria-hidden": "true" }, lines.map((w) => h("span", { class: "skeleton-line", style: { width: `${w}%` } })));
}

export function spinner(cls = ""): HTMLElement {
  return h("span", { class: `spinner ${cls}`, "aria-hidden": "true" });
}

/** A labelled card section with an id'd heading, for aria-labelledby. */
export function card(title: string, opts: { icon?: IconName; cls?: string; actions?: Child; kicker?: string } = {}, ...body: Child[]): HTMLElement {
  const id = nextId("card");
  return h(
    "section",
    { class: `card ${opts.cls ?? ""}`, "aria-labelledby": id },
    h(
      "header",
      { class: "card-head" },
      opts.icon ? h("span", { class: "card-icon" }, icon(opts.icon)) : null,
      h("div", { class: "card-titles" }, opts.kicker ? h("p", { class: "card-kicker", text: opts.kicker }) : null, h("h2", { id, class: "card-title", text: title })),
      opts.actions ? h("div", { class: "card-actions" }, opts.actions) : null,
    ),
    h("div", { class: "card-body" }, body),
  );
}
