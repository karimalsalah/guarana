// Circular progress for installs. stroke-dashoffset on one small SVG; indeterminate while unknown.

import { h, nextId, svg } from "../dom";
import { icon } from "../icons";

export interface ProgressRing {
  el: HTMLElement;
  set(fraction: number | null, label?: string): void;
  done(ok: boolean): void;
  reset(): void;
}

export function progressRing(label: string): ProgressRing {
  const gid = nextId("ring");
  const r = 44;
  const c = 2 * Math.PI * r;
  const ring = svg(
    `<svg class="ring-svg" viewBox="0 0 100 100" aria-hidden="true">
      <defs><linearGradient id="${gid}" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#FFC94A"/><stop offset="1" stop-color="#F2851A"/></linearGradient></defs>
      <circle class="ring-track" cx="50" cy="50" r="${r}" fill="none" stroke-width="7"/>
      <circle class="ring-bar" cx="50" cy="50" r="${r}" fill="none" stroke="url(#${gid})" stroke-width="7" stroke-linecap="round"
        stroke-dasharray="${c.toFixed(2)}" stroke-dashoffset="${c.toFixed(2)}" transform="rotate(-90 50 50)"/>
    </svg>`,
  );
  const bar = ring.querySelector<SVGCircleElement>(".ring-bar");
  const pct = h("span", { class: "ring-pct", text: "" });
  const center = h("span", { class: "ring-center" }, pct);
  const el = h(
    "div",
    { class: "ring is-indeterminate", role: "progressbar", "aria-label": label, "aria-valuemin": "0", "aria-valuemax": "100" },
    ring,
    center,
  );
  return {
    el,
    set(fraction, text) {
      if (fraction === null || !Number.isFinite(fraction)) {
        el.classList.add("is-indeterminate");
        el.removeAttribute("aria-valuenow");
        pct.textContent = "";
      } else {
        const f = Math.min(1, Math.max(0, fraction));
        el.classList.remove("is-indeterminate");
        bar?.setAttribute("stroke-dashoffset", (c * (1 - f)).toFixed(2));
        el.setAttribute("aria-valuenow", String(Math.round(f * 100)));
        pct.textContent = `${Math.round(f * 100)}%`;
      }
      if (text) el.setAttribute("aria-valuetext", text);
    },
    done(ok) {
      el.classList.remove("is-indeterminate");
      el.classList.add(ok ? "is-done" : "is-failed");
      bar?.setAttribute("stroke-dashoffset", ok ? "0" : (c * 0.999).toFixed(2));
      center.replaceChildren(icon(ok ? "check" : "close", "ring-icon"));
      el.setAttribute("aria-valuenow", ok ? "100" : "0");
    },
    reset() {
      el.classList.remove("is-done", "is-failed");
      el.classList.add("is-indeterminate");
      bar?.setAttribute("stroke-dashoffset", c.toFixed(2));
      center.replaceChildren(pct);
      pct.textContent = "";
    },
  };
}
