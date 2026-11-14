// Updates: installed vs latest, release notes (safe Markdown), stable/nightly, install with a
// progress ring (install://progress), and the installed versions to roll back to.

import { api, errorText, events } from "../api";
import { h } from "../dom";
import { bytes, installText, relativeTime, shortDate, v } from "../format";
import { icon } from "../icons";
import { settleFocus } from "../input";
import { type ScreenDef } from "../router";
import { launcher } from "../state";
import type { Channel, InstalledVersion, LauncherState, Progress, UpdateInfo } from "../types";
import { badge, busy, button, card, segmented, skeleton, type Segmented } from "../ui/controls";
import { progressRing } from "../ui/progress-ring";
import { toast } from "../ui/toast";
import { resetUpdateCheck } from "./home";

const CHANNEL_KEY = "guarana.channel";

function savedChannel(): Channel {
  try {
    return localStorage.getItem(CHANNEL_KEY) === "nightly" ? "nightly" : "stable";
  } catch {
    return "stable";
  }
}

/** After an install or a switch: the footer rereads the version, and Home checks for updates again. */
function announceState(): void {
  resetUpdateCheck();
  document.dispatchEvent(new CustomEvent("guarana:state"));
}


const screen: ScreenDef = {
  title: "Updates",
  chrome: "full",
  tab: "updates",

  render(ctx) {
    let channel: Channel = savedChannel();
    let info: UpdateInfo | null = null;
    let installing = false;

    const nightlyNote = h("p", { class: "channel-note", hidden: channel !== "nightly" }, icon("warn"), "Nightlies are built every day from main and can break. Your current version stays installed.");
    const channelSwitch: Segmented = segmented({
      label: "Update channel",
      value: channel,
      choices: [
        { value: "stable", label: "Stable" },
        { value: "nightly", label: "Nightly" },
      ],
      onChange: (value) => {
        if (installing) {
          channelSwitch.set(channel);
          return;
        }
        channel = value === "nightly" ? "nightly" : "stable";
        nightlyNote.hidden = channel !== "nightly";
        void check();
      },
    });

    const installedVer = h("p", { class: "ver-num", text: "–" });
    const latestLabel = h("p", { class: "ver-label", text: "Latest" });
    const latestVer = h("p", { class: "ver-num", text: "–" });
    const latestMeta = h("p", { class: "ver-meta" }, skeleton([60]));
    const ring = progressRing("Installing");
    ring.el.hidden = true;
    const status = h("p", { class: "update-status", role: "status" });
    const action = button("Checking…", { kind: "primary", size: "lg", icon: "download", primary: true, attrs: { disabled: true } });
    const updateCard = h(
      "section",
      { class: "card update-card", "aria-label": "Installed and latest version" },
      h(
        "div",
        { class: "versions" },
        h("div", { class: "ver" }, h("p", { class: "ver-label", text: "Installed" }), installedVer),
        h("span", { class: "ver-arrow", "aria-hidden": "true" }, icon("arrowRight")),
        h("div", { class: "ver ver-latest" }, latestLabel, latestVer, latestMeta),
      ),
      h("div", { class: "update-action" }, ring.el, h("div", { class: "update-cta" }, status, action)),
    );

    const notesTitle = h("h2", { class: "card-title", id: "notes-title", text: "What's new" });
    const notesBody = h("div", { class: "md notes-body" }, skeleton([90, 70, 80, 55]));
    const notes = h("section", { class: "card notes", "aria-labelledby": "notes-title" }, h("header", { class: "card-head" }, h("span", { class: "card-icon" }, icon("sparkle")), h("div", { class: "card-titles" }, notesTitle)), notesBody);
    const versionsList = h("ul", { class: "versions-list" }, skeleton([80, 70, 75]));
    const rollback = card("Installed versions", { icon: "history", kicker: "Roll back any time" }, versionsList, h("p", { class: "fine", text: "The version you pick is the one Play starts next." }));

    const el = h(
      "div",
      { class: "updates" },
      h(
        "header",
        { class: "screen-head has-actions" },
        h("p", { class: "eyebrow", text: "Updates" }),
        h("h1", { text: "Stay current" }),
        h("p", { class: "lede", text: "Updates come only from Guarana's GitHub Releases, and each download is checked against its SHA-256." }),
        h("div", { class: "head-actions channel" }, h("span", { class: "channel-label", text: "Channel" }), channelSwitch.el),
      ),
      nightlyNote,
      updateCard,
      h("div", { class: "updates-grid" }, notes, rollback),
    );

    // Install progress: listen once, before any install starts.
    // Stages from the contract: resolve, download (bytes), verify, extract (files), done.
    const listening = events.installProgress((p: Progress) => {
      if (!installing) return;
      const text = installText(p);
      ring.set(p.stage === "download" && p.total > 0 ? p.done / p.total : p.stage === "done" ? 1 : null, text);
      status.textContent = text;
    }, ctx.signal);

    const renderInstalled = (state: LauncherState) => {
      installedVer.textContent = v(state.current);
      const sorted = [...state.installed].sort((a, b) => Date.parse(b.installed_at) - Date.parse(a.installed_at));
      versionsList.replaceChildren(...(sorted.length ? sorted.map((iv) => versionRow(iv, state)) : [h("li", { class: "fine", text: "No versions installed yet." })]));
    };

    const versionRow = (iv: InstalledVersion, state: LauncherState): HTMLElement => {
      const isCurrent = iv.version === state.current;
      const use = isCurrent
        ? badge("In use", "ok", "ok")
        : button("Use this version", {
            kind: "secondary",
            size: "sm",
            onClick: (_e, self) =>
              void busy(self, () => api.setCurrentVersion(iv.version), "Switching…")
                .then((next) => {
                  launcher.set(next);
                  renderInstalled(next);
                  announceState();
                  toast(`Now using ${v(iv.version)}.`, { tone: "ok" });
                  if (info) renderInfo(info, next);
                })
                .catch((e: unknown) => toast(errorText(e), { tone: "fail" })),
          });
      return h(
        "li",
        { class: `version-row${isCurrent ? " is-current" : ""}` },
        h("span", { class: "item-icon" }, icon(isCurrent ? "ok" : "history")),
        h("div", { class: "grow" }, h("p", { class: "item-title" }, v(iv.version), " ", h("span", { class: "muted", text: iv.channel })), h("p", { class: "item-sub", text: `Installed ${relativeTime(iv.installed_at)}` })),
        use,
      );
    };

    const renderInfo = (u: UpdateInfo, state: LauncherState | null) => {
      latestLabel.textContent = u.channel === "nightly" ? "Latest nightly" : "Latest stable";
      latestVer.textContent = v(u.latest);
      latestMeta.textContent = `Released ${shortDate(u.published_at)} · ${bytes(u.size_bytes)}`;
      const isCurrent = (state?.current ?? u.current) === u.latest;
      const alreadyInstalled = state?.installed.some((x) => x.version === u.latest) ?? false;
      const label = action.querySelector(".btn-label");
      updateCard.classList.toggle("is-current", isCurrent);
      if (isCurrent) {
        action.disabled = true;
        if (label) label.textContent = "You're up to date";
        status.textContent = "";
      } else if (alreadyInstalled) {
        action.disabled = false;
        if (label) label.textContent = `Use ${v(u.latest)}`;
        status.textContent = "Already downloaded.";
      } else {
        action.disabled = false;
        if (label) label.textContent = `Install ${v(u.latest)}`;
        status.textContent = u.update_available ? `${bytes(u.size_bytes)} from GitHub Releases` : "";
      }
      notesTitle.textContent = `What's new in ${v(u.latest)}`;
      void import("../markdown").then((m) => {
        if (!ctx.signal.aborted) notesBody.replaceChildren(m.renderMarkdown(u.notes_md || "No notes for this release."));
      });
    };

    const check = async () => {
      const asked = channel;
      latestVer.textContent = "–";
      latestMeta.replaceChildren(skeleton([60]));
      action.disabled = true;
      const label = action.querySelector(".btn-label");
      if (label) label.textContent = "Checking…";
      try {
        const [u, state] = await Promise.all([api.checkUpdates(asked), launcher.get(true)]);
        if (ctx.signal.aborted || asked !== channel) return;
        info = u;
        if (u.channel !== asked) {
          channel = u.channel === "nightly" ? "nightly" : "stable";
          channelSwitch.set(channel);
          nightlyNote.hidden = channel !== "nightly";
          toast(`The ${asked} channel isn't available from this launcher yet.`, { tone: "info" });
        } else {
          try {
            localStorage.setItem(CHANNEL_KEY, asked);
          } catch {
            /* ignore */
          }
        }
        renderInfo(u, state);
        renderInstalled(state);
        settleFocus(el);
      } catch (e) {
        if (ctx.signal.aborted) return;
        latestMeta.textContent = "";
        status.textContent = errorText(e);
        if (label) label.textContent = "Try again";
        action.disabled = false;
        info = null;
        launcher.get().then(renderInstalled, () => undefined);
      }
    };

    action.addEventListener("click", () => {
      if (!info) {
        void check();
        return;
      }
      const u = info;
      void (async () => {
        const state = launcher.peek();
        if (state?.installed.some((x) => x.version === u.latest)) {
          const next = await api.setCurrentVersion(u.latest);
          launcher.set(next);
          renderInstalled(next);
          renderInfo(u, next);
          announceState();
          toast(`Now using ${v(u.latest)}.`, { tone: "ok" });
          return;
        }
        installing = true;
        action.disabled = true;
        ring.reset();
        ring.el.hidden = false;
        updateCard.classList.add("is-installing");
        status.textContent = "Starting the download…";
        try {
          await listening;
          const r = await api.installVersion(u.latest, channel);
          ring.done(true);
          status.textContent = `Installed ${v(r.version)}${r.verified_sha256 ? " · SHA-256 verified" : ""} · ${Math.round(r.seconds)} s`;
          const next = await api.setCurrentVersion(r.version);
          launcher.set(next);
          renderInstalled(next);
          announceState();
          toast(`${v(r.version)} is ready. Play starts it next time.`, { tone: "ok" });
          const label = action.querySelector(".btn-label");
          if (label) label.textContent = "You're up to date";
          updateCard.classList.add("is-current");
        } catch (e) {
          ring.done(false);
          status.textContent = errorText(e);
          action.disabled = false;
          const label = action.querySelector(".btn-label");
          if (label) label.textContent = "Try again";
        } finally {
          installing = false;
          updateCard.classList.remove("is-installing");
        }
      })().catch((e: unknown) => toast(errorText(e), { tone: "fail" }));
    });

    void check();
    return el;
  },
};

export default screen;
