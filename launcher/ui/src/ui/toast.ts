// Short notices with an optional action. Polite live region; never steals focus.

import { h } from "../dom";
import { icon, type IconName } from "../icons";

export type Tone = "ok" | "warn" | "fail" | "info";

export interface ToastOptions {
  tone?: Tone;
  action?: { label: string; run: () => void };
  /** ms; 0 keeps it until closed. Defaults to 5 s, 9 s with an action. */
  timeout?: number;
}

const ICONS: Record<Tone, IconName> = { ok: "ok", warn: "warn", fail: "fail", info: "info" };

export function toast(message: string, opts: ToastOptions = {}): () => void {
  const region = document.querySelector<HTMLElement>(".toasts");
  if (!region) return () => undefined;
  const tone = opts.tone ?? "info";
  let timer = 0;
  const close = () => {
    window.clearTimeout(timer);
    el.classList.add("is-leaving");
    const done = () => el.remove();
    el.addEventListener("animationend", done, { once: true });
    window.setTimeout(done, 400);
  };
  const el = h(
    "div",
    { class: `toast toast-${tone}`, role: tone === "fail" ? "alert" : "status" },
    h("span", { class: "toast-icon" }, icon(ICONS[tone])),
    h("p", { class: "toast-text", text: message }),
    opts.action
      ? h("button", { type: "button", class: "btn btn-sm btn-ghost", onClick: () => { opts.action?.run(); close(); } }, opts.action.label)
      : null,
    h("button", { type: "button", class: "icon-btn icon-btn-sm", "aria-label": "Dismiss", onClick: close }, icon("close")),
  );
  while (region.children.length >= 3) region.firstElementChild?.remove();
  region.appendChild(el);
  const ms = opts.timeout ?? (opts.action ? 9000 : 5000);
  const arm = () => {
    if (ms > 0) timer = window.setTimeout(close, ms);
  };
  el.addEventListener("pointerenter", () => window.clearTimeout(timer));
  el.addEventListener("pointerleave", arm);
  el.addEventListener("focusin", () => window.clearTimeout(timer));
  arm();
  return close;
}
