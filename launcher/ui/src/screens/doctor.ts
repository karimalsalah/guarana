// Doctor: every check with its status and fix, run again, and "Copy diagnostics" (scrubbed text).

import { api, errorText, events } from "../api";
import { h } from "../dom";
import { gpuName, plural, scrub, v } from "../format";
import { icon } from "../icons";
import { settleFocus } from "../input";
import { go, type ScreenDef } from "../router";
import { checks, launcher, systemInfo } from "../state";
import type { Check, LauncherState, Progress, SystemInfo } from "../types";
import { checkRow, sortChecks, summarize } from "../ui/checks";
import { badge, busy, button, skeleton } from "../ui/controls";
import { askAiButtons } from "../ui/ask-ai";
import { toast } from "../ui/toast";

const STATUS_WORD: Record<Check["status"], string> = { ok: "OK", warn: "CHECK", fail: "FIX", info: "INFO" };

async function copyText(text: string): Promise<void> {
  try {
    await navigator.clipboard.writeText(text);
    return;
  } catch {
    const ta = h("textarea", { class: "sr-only", "aria-hidden": "true" });
    ta.value = text;
    document.body.appendChild(ta);
    ta.select();
    const ok = document.execCommand("copy");
    ta.remove();
    if (!ok) throw new Error("The clipboard isn't available.");
  }
}

export function diagnosticsText(list: Check[], sys: SystemInfo | undefined, state: LauncherState | undefined, launcherVersion: string): string {
  const lines: string[] = ["## Guarana diagnostics", ""];
  lines.push(`- Launcher: ${v(launcherVersion)}`);
  if (state) lines.push(`- Guarana: ${v(state.current)} (${state.installed.find((x) => x.version === state.current)?.channel ?? "?"}), profile: ${state.profile_id ?? "none"}`);
  if (sys) {
    const gpu = sys.gpus[sys.primary_gpu] ?? sys.gpus[0];
    lines.push(`- Device: ${sys.device.manufacturer} ${sys.device.model} (${sys.device.form})`);
    lines.push(`- OS: ${sys.os.name} ${sys.os.display_version} (build ${sys.os.build})`);
    lines.push(`- CPU: ${sys.cpu.name}, ${sys.cpu.cores} cores / ${sys.cpu.threads} threads, AVX2 ${sys.cpu.avx2 ? "yes" : "no"}`);
    if (gpu) lines.push(`- GPU: ${gpuName(gpu.name)}, driver ${gpu.driver_version}, ${gpu.is_integrated ? "integrated" : "dedicated"}, Vulkan ${gpu.vulkan ? "yes" : "no"}`);
    lines.push(`- Memory: ${sys.ram_gb} GB RAM, page file ${sys.pagefile_gb} GB`);
    lines.push(`- Display: ${sys.display.width} x ${sys.display.height} at ${sys.display.refresh_hz} Hz, VRR ${sys.display.vrr ?? "?"}, HDR ${sys.display.hdr ?? "?"}`);
    lines.push(`- Power: ${sys.power.on_ac === null ? "?" : sys.power.on_ac ? "AC" : "battery"}, plan ${sys.power.plan ?? "?"}, HAGS ${sys.hags ?? "?"}`);
  }
  lines.push("", "### Checks", "");
  for (const c of sortChecks(list)) lines.push(`- [${STATUS_WORD[c.status]}] ${c.title}: ${c.detail}`);
  return scrub(lines.join("\n"));
}

const screen: ScreenDef = {
  title: "Doctor",
  chrome: "full",
  tab: "doctor",

  render(ctx) {
    let list: Check[] = checks.value ?? [];
    let running = false;

    const summary = h("div", { class: "row doctor-summary", "aria-live": "polite" });
    const progressText = h("p", { class: "doctor-progress-text", text: "Starting checks…" });
    const progressFill = h("span", { class: "doctor-progress-fill" });
    // Always in the layout (the bar fades), so finishing a run never shifts the list under the pointer.
    const progress = h("div", { class: "doctor-progress is-done" }, h("div", { class: "doctor-progress-bar", "aria-hidden": "true" }, progressFill), progressText);
    const copyFallback = h("div", { class: "copy-fallback", hidden: true });
    const listEl = h("ul", { class: "checks" }, skeleton([80, 70, 85, 60, 75], "skeleton-tall"));
    const rerun = button("Run again", { kind: "secondary", size: "md", icon: "refresh" });
    const copy = button("Copy diagnostics", { kind: "primary", size: "md", icon: "copy", primary: true });

    const el = h(
      "div",
      { class: "doctor" },
      h(
        "header",
        { class: "screen-head has-actions" },
        h("p", { class: "eyebrow", text: "Doctor" }),
        h("h1", { text: "Health check" }),
        h("p", { class: "lede", text: "Everything Guarana needs, checked on this PC. Fixes open the right Windows or vendor page, tidy Guarana's own folders or update Guarana. Nothing else is changed for you." }),
        h("div", { class: "head-actions" }, rerun, copy),
      ),
      h("div", { class: "doctor-ai" }, h("p", { class: "fine", text: "Stuck? Ask an AI assistant. A short summary of this PC (no user or PC name) is typed into a chat for you; you read it before sending." }), askAiButtons(() => list.filter((c) => c.status === "fail" || c.status === "warn").map((c) => `${c.title}: ${c.detail}`), () => undefined)),
      copyFallback,
      progress,
      summary,
      h("section", { class: "card checks-card", "aria-label": "Checks" }, listEl),
      h("p", { class: "fine doctor-foot" }, "Something else wrong? ", h("button", { type: "button", class: "link-btn", onClick: () => void go("report") }, "Report a problem", icon("chevronRight"))),
    );

    const renderSummary = () => {
      const s = summarize(list);
      summary.replaceChildren(
        ...[
          s.fail ? badge(`${s.fail} to fix`, "fail", "fail") : null,
          s.warn ? badge(`${s.warn} to check`, "warn", "warn") : null,
          s.info ? badge(`${s.info} for your info`, "info", "info") : null,
          badge(`${s.ok} good`, "ok", "ok"),
        ].filter((x): x is HTMLElement => x !== null),
      );
    };

    const renderList = (animate: boolean) => {
      renderSummary();
      listEl.replaceChildren(
        ...sortChecks(list).map((c, i) => {
          const row = checkRow(c, {
            onFixed: (u) => {
              list = list.map((x) => (x.id === u.id ? u : x));
              checks.set(list);
              renderSummary();
            },
          });
          if (animate) {
            row.classList.add("check-reveal");
            row.style.setProperty("--i", String(i));
          }
          return row;
        }),
      );
    };

    // doctor://progress: stage is the check id being run (message its title), then "done".
    const listening = events.doctorProgress((p: Progress) => {
      if (!running) return;
      progressFill.style.transform = `scaleX(${p.total ? p.done / p.total : 0})`;
      if (p.stage !== "done") progressText.textContent = `${p.message ?? "Checking"}… ${Math.min(p.done + 1, p.total)} of ${p.total}`;
    }, ctx.signal);

    const run = async () => {
      running = true;
      progress.classList.remove("is-done");
      progressFill.style.transform = "scaleX(0)";
      progressText.textContent = "Starting checks…";
      try {
        await listening;
        list = await api.runDoctor();
        if (ctx.signal.aborted) return;
        checks.set(list);
        renderList(true);
        settleFocus(el);
        const s = summarize(list);
        progressText.textContent = s.fail + s.warn ? `${plural(s.fail + s.warn, "thing", "things")} to look at.` : "All good.";
      } catch (e) {
        if (ctx.signal.aborted) return;
        listEl.replaceChildren(h("li", { class: "check" }, h("p", { class: "fine", text: `Checks didn't finish: ${errorText(e)}` })));
      } finally {
        running = false;
        progressFill.style.transform = "scaleX(1)";
        setTimeout(() => {
          if (!running) progress.classList.add("is-done");
        }, 700);
      }
    };

    rerun.addEventListener("click", () => void busy(rerun, run, "Checking…"));
    copy.addEventListener("click", () => {
      void (async () => {
        const [sys, state, version] = await Promise.all([
          systemInfo.get().catch(() => undefined),
          launcher.get().catch(() => undefined),
          api.appVersion().catch(() => "?"),
        ]);
        const text = diagnosticsText(list, sys, state, version);
        try {
          await copyText(text);
          copyFallback.hidden = true;
          toast("Diagnostics copied, without your user name. Paste them into an issue or a Discussions post.", { tone: "ok" });
        } catch {
          // No clipboard access: show the text, selected, so Ctrl+C works.
          const area = h("textarea", { class: "textarea mono", readonly: true, rows: "8", "aria-label": "Diagnostics" });
          area.value = text;
          copyFallback.replaceChildren(h("p", { class: "fine", text: "Copying didn't work here. The text is selected below: press Ctrl+C." }), area);
          copyFallback.hidden = false;
          area.focus();
          area.select();
        }
      })().catch((e: unknown) => toast(`Couldn't build diagnostics: ${errorText(e)}`, { tone: "fail" }));
    });

    if (list.length) renderList(false);
    void run();
    return el;
  },
};

export default screen;
