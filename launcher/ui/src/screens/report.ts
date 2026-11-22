// Report a problem: pick a kind, add a note, build the report on this PC (collect_report), read the
// summary, then open the prefilled GitHub issue (open_url). Nothing is posted until you submit there.

import { api, errorText } from "../api";
import { h } from "../dom";
import { basename, dirname, prettyPath } from "../format";
import { icon, type IconName } from "../icons";
import { type ScreenDef } from "../router";
import type { Report, ReportKind } from "../types";
import { busy, button } from "../ui/controls";
import { askAiButtons } from "../ui/ask-ai";
import { toast } from "../ui/toast";

const KINDS: { id: ReportKind; label: string; hint: string; icon: IconName }[] = [
  { id: "bug", label: "Bug", hint: "Something looks or works wrong", icon: "bug" },
  { id: "performance", label: "Performance", hint: "Slow, stutters or drops frames", icon: "gauge" },
  { id: "crash", label: "Crash", hint: "Guarana closed or froze", icon: "bolt" },
];

const MAX_NOTE = 2000;

const screen: ScreenDef = {
  title: "Report a problem",
  chrome: "full",

  render(ctx) {
    const preset = ctx.params.kind;
    let kind: ReportKind = preset === "performance" || preset === "crash" || preset === "bug" ? preset : "bug";
    let report: Report | null = null;

    const kindButtons: HTMLButtonElement[] = KINDS.map((k) => {
      const b = h(
        "button",
        { type: "button", class: "kind-card", "aria-pressed": String(k.id === kind), "data-kind": k.id },
        h("span", { class: "kind-icon" }, icon(k.icon)),
        h("span", { class: "kind-label", text: k.label }),
        h("span", { class: "kind-hint", text: k.hint }),
      );
      b.addEventListener("click", () => {
        kind = k.id;
        for (const x of kindButtons) x.setAttribute("aria-pressed", String(x.dataset.kind === k.id));
        if (report) stale();
      });
      return b;
    });

    const note = h("textarea", {
      id: "report-note",
      class: "textarea",
      maxlength: String(MAX_NOTE),
      placeholder: "What were you doing? What did you expect to happen?",
      "aria-describedby": "report-note-hint",
    });
    const presetNote = ctx.params.note;
    if (typeof presetNote === "string") note.value = presetNote.slice(0, MAX_NOTE);
    const count = h("span", { class: "field-count", "aria-hidden": "true" });
    const syncCount = () => (count.textContent = `${note.value.length} / ${MAX_NOTE}`);
    syncCount();
    note.addEventListener("input", () => {
      syncCount();
      if (report) stale();
    });

    const build = button("Build report", { kind: "primary", size: "lg", icon: "flag", primary: true });
    const open = button("Open GitHub issue", { kind: "primary", size: "lg", icon: "external" });
    const summary = h("div", { class: "md report-summary" });
    const fileTitle = h("p", { class: "item-title" });
    const fileSub = h("p", { class: "item-sub" });
    const showFile = button("Show file", { kind: "ghost", size: "sm", icon: "folder" });
    const result = h(
      "section",
      { class: "card report-result", "aria-labelledby": "report-ready", hidden: true },
      h("header", { class: "card-head" }, h("span", { class: "card-icon card-icon-ok" }, icon("ok")), h("div", { class: "card-titles" }, h("p", { class: "card-kicker", text: "Built on this PC" }), h("h2", { class: "card-title", id: "report-ready", text: "Your report is ready" }))),
      summary,
      h("div", { class: "item" }, h("span", { class: "item-icon" }, icon("folder")), h("div", { class: "grow" }, fileTitle, fileSub), showFile),
      h("div", { class: "row" }, open),
      h("p", { class: "fine", text: "Or ask an AI assistant first. It gets a short scrubbed summary and the link to Guarana's docs." }),
      askAiButtons(() => [], () => note.value.trim() || undefined),
      h("p", { class: "fine" }, "This opens your browser with the issue filled in. Drag the .zip into it to attach the logs. Nothing is posted until you press Submit on GitHub."),
    );

    const el = h(
      "div",
      { class: "report" },
      h(
        "header",
        { class: "screen-head" },
        h("p", { class: "eyebrow", text: "Report a problem" }),
        h("h1", { text: "What went wrong?" }),
        h("p", { class: "lede", text: "The report is built on your PC. You read it on GitHub before anything is posted." }),
      ),
      h("div", { class: "kind-cards", role: "group", "aria-label": "Kind of problem" }, kindButtons),
      h(
        "div",
        { class: "field" },
        h("label", { class: "field-label", for: "report-note" }, "What happened? ", h("span", { class: "muted", text: "(optional)" })),
        note,
        h("div", { class: "field-foot" }, h("p", { class: "field-hint", id: "report-note-hint", text: "No passwords or personal details, please. Your user name and PC name are removed from the logs." }), count),
      ),
      h("div", { class: "row report-actions" }, build),
      result,
    );

    // While typing a note, B / Esc leaves the field instead of the screen.
    ctx.onIntent((intent) => {
      if (intent === "back" && document.activeElement === note) {
        note.blur();
        build.focus();
        return true;
      }
      return false;
    });

    const setPrimary = (btn: HTMLButtonElement, other: HTMLButtonElement) => {
      btn.dataset.primary = "";
      delete other.dataset.primary;
      btn.classList.replace("btn-secondary", "btn-primary");
      other.classList.replace("btn-primary", "btn-secondary");
    };

    function stale() {
      report = null;
      result.hidden = true;
      const label = build.querySelector(".btn-label");
      if (label) label.textContent = "Build report";
      setPrimary(build, open);
    }

    build.addEventListener("click", () => {
      void busy(build, () => api.collectReport(kind, note.value.trim() || undefined), "Building…")
        .then((r) => {
          if (ctx.signal.aborted) return;
          report = r;
          void import("../markdown").then((m) => summary.replaceChildren(m.renderMarkdown(r.summary_md, 4)));
          fileTitle.textContent = basename(r.zip_path);
          fileSub.textContent = prettyPath(dirname(r.zip_path));
          result.hidden = false;
          const label = build.querySelector(".btn-label");
          if (label) label.textContent = "Build again";
          setPrimary(open, build);
          open.focus({ preventScroll: true });
          result.scrollIntoView({ block: "nearest", behavior: "smooth" });
        })
        .catch((e: unknown) => toast(`Couldn't build the report: ${errorText(e)}`, { tone: "fail" }));
    });

    showFile.addEventListener("click", () => {
      if (report) void api.openPath(report.zip_path).catch((e: unknown) => toast(errorText(e), { tone: "fail" }));
    });

    open.addEventListener("click", () => {
      if (!report) return;
      void api
        .openUrl(report.issue_url)
        .then(() => toast("Opened in your browser. Review it, attach the .zip if you like, then submit.", { tone: "ok" }))
        .catch((e: unknown) => toast(errorText(e), { tone: "fail" }));
    });

    setPrimary(build, open);
    return el;
  },
};

export default screen;
