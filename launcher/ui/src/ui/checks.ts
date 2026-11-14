// A doctor check as a row: status (icon + word), title, detail, optional Fix button.

import { api, errorText, events, type Unlisten } from "../api";
import { fill, h } from "../dom";
import { installText } from "../format";
import { launcher } from "../state";
import type { Check, CheckStatus } from "../types";
import { busy, button, statusPill } from "./controls";
import { toast } from "./toast";

export const STATUS_ORDER: Record<CheckStatus, number> = { fail: 0, warn: 1, info: 2, ok: 3 };

export function sortChecks(list: Check[]): Check[] {
  return [...list].sort((a, b) => STATUS_ORDER[a.status] - STATUS_ORDER[b.status]);
}

export function summarize(list: Check[]): { fail: number; warn: number; info: number; ok: number } {
  const s = { fail: 0, warn: 0, info: 0, ok: 0 };
  for (const c of list) s[c.status] += 1;
  return s;
}

/** Runs a fix. `install_update` streams install://progress, shown on the button while it runs. */
async function runFix(fixId: string, btn: HTMLButtonElement): Promise<Check> {
  if (fixId !== "install_update") return api.applyFix(fixId);
  const label = btn.querySelector<HTMLElement>(".btn-label");
  let unlisten: Unlisten | null = null;
  try {
    unlisten = await events.installProgress((p) => {
      if (label) label.textContent = installText(p);
    });
    const updated = await api.applyFix(fixId);
    void launcher.get(true).then(() => document.dispatchEvent(new CustomEvent("guarana:state")));
    return updated;
  } finally {
    unlisten?.();
  }
}

/** Renders a check. `onFixed` gets the Check that apply_fix returned (the check run again). */
export function checkRow(check: Check, opts: { compact?: boolean; onFixed?: (c: Check) => void } = {}): HTMLElement {
  const row = h("li", { class: `check check-${check.status}${opts.compact ? " is-compact" : ""}`, "data-check": check.id });
  const render = (c: Check) => {
    row.className = `check check-${c.status}${opts.compact ? " is-compact" : ""}`;
    const fixId = c.fix_id;
    const fix =
      fixId && c.fix_label
        ? button(c.fix_label, {
            kind: c.status === "fail" ? "primary" : "secondary",
            size: "sm",
            onClick: (_e, self) => {
              const before = c.status;
              void busy(self, () => runFix(fixId, self), fixId === "install_update" ? "Starting…" : "Working…")
                .then((updated) => {
                  render(updated);
                  opts.onFixed?.(updated);
                  // Only the open_* fixes open a page. The others (install_update, install_vc_redist,
                  // delete_shader_tmp) put what they did at the start of the check's detail.
                  if (!fixId.startsWith("open_")) toast(updated.detail, { tone: updated.status === "warn" || updated.status === "fail" ? "warn" : "info" });
                  else if (updated.status === "ok" && before !== "ok") toast(`${updated.title}: done.`, { tone: "ok" });
                  else toast("Opened. Finish there, then run the checks again.", { tone: "info" });
                })
                .catch((err: unknown) => toast(`${c.title}: ${errorText(err)}`, { tone: "fail" }));
            },
          })
        : null;
    fill(
      row,
      statusPill(c.status),
      h("div", { class: "check-text" }, h("p", { class: "check-title", text: c.title }), h("p", { class: "check-detail", text: c.detail })),
      fix ? h("div", { class: "check-fix" }, fix) : null,
    );
  };
  render(check);
  return row;
}
