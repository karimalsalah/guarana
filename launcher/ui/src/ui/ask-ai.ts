// "Ask AI": builds a scrubbed summary on this PC (ask_ai) and opens a prefilled chat in the browser.
// The prompt points the assistant at Guarana's docs; the user reads it in the chat before sending.

import { api, errorText } from "../api";
import { h } from "../dom";
import type { AiService } from "../types";
import { busy, button } from "./controls";
import { toast } from "./toast";

export const DISCORD_URL = "https://discord.gg/urz7vMdRM";
export const AI_QUICKSTART_URL = "https://github.com/karimalsalah/guarana/blob/main/docs/AI-QUICKSTART.md";

export function askAiButtons(getIssues: () => string[], getQuestion: () => string | undefined): HTMLElement {
  const make = (service: AiService, label: string) =>
    button(label, {
      kind: "secondary",
      size: "md",
      icon: "chat",
      attrs: { title: "Opens a chat in your browser with a short summary of this PC already typed in. Nothing is sent until you press send there." },
      onClick: (_e, self) =>
        void busy(self, () => api.askAi(service, getIssues(), getQuestion()), "Opening…")
          .then(() => toast(`Opened ${label.replace("Ask ", "")} in your browser. Read the question, then send it.`, { tone: "ok" }))
          .catch((e: unknown) => toast(`Couldn't open the chat: ${errorText(e)}`, { tone: "fail" })),
    });
  return h("div", { class: "row ask-ai", role: "group", "aria-label": "Ask an AI assistant" }, make("claude", "Ask Claude"), make("chatgpt", "Ask ChatGPT"));
}
