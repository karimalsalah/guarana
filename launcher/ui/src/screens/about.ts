// About / Credits: everyone in the website's hall of fame (get_credits), people first and AI tools
// apart, and the way to get a name on it. Every name is a text node; a login links to its GitHub page.

import { api, errorText } from "../api";
import { h } from "../dom";
import { type ScreenDef } from "../router";
import type { CreditPerson, Credits } from "../types";
import { button, skeleton } from "../ui/controls";
import { AI_QUICKSTART_URL, DISCORD_URL } from "../ui/ask-ai";
import { toast } from "../ui/toast";

export const CONTRIBUTING_URL = "https://github.com/karimalsalah/guarana/blob/main/CONTRIBUTING.md";
const HALL_URL = "https://karimalsalah.github.io/guarana/legacy.html";

function open(url: string): void {
  void api.openUrl(url).catch((e: unknown) => toast(`Couldn't open the link: ${errorText(e)}`, { tone: "fail" }));
}

function plural(n: number, one: string, many: string): string {
  return `${n} ${n === 1 ? one : many}`;
}

function personRow(p: CreditPerson): HTMLElement {
  const label = p.login ?? p.name ?? "";
  const what = [p.commits ? plural(p.commits, "commit", "commits") : "", p.coauthored ? `co-author of ${p.coauthored}` : ""].filter(Boolean).join(" · ");
  const body = [h("span", { class: "item-title", text: label }), h("span", { class: "item-sub", text: what })];
  if (!p.login) return h("li", { class: "item credit" }, h("div", { class: "grow" }, body));
  const login = p.login;
  return h(
    "li",
    null,
    h(
      "button",
      { type: "button", class: "item credit credit-link", "aria-label": `${label} on GitHub`, onClick: () => open(`https://github.com/${login}`) },
      h("div", { class: "grow" }, body),
    ),
  );
}

function render(body: HTMLElement, c: Credits): void {
  const people = h("ul", { class: "items credit-list", "aria-label": "People" }, c.people.map(personRow));
  const tools = h(
    "ul",
    { class: "items credit-list", "aria-label": "Tools" },
    c.tools.map((t) =>
      h("li", { class: "item credit" }, h("div", { class: "grow" }, h("span", { class: "item-title", text: t.name }), h("span", { class: "item-sub", text: `helped with ${plural(t.commits, "commit", "commits")}` }))),
    ),
  );
  const parts: (HTMLElement | null)[] = [
    h("h2", { class: "section-title", text: `People (${c.people.length})` }),
    c.people.length ? people : h("p", { class: "fine", text: "Nobody yet: the first name can be yours." }),
    c.tools.length
      ? h(
          "div",
          { class: "credit-tools" },
          h("h2", { class: "section-title", text: "Tools" }),
          h("p", { class: "fine", text: "AI tools and bots that helped are listed apart from the people: a person owns every change." }),
          tools,
        )
      : null,
    h("p", { class: "fine", text: `${plural(c.commits, "commit", "commits")}${c.as_of ? ` up to ${c.as_of}` : ""}. Same list as the hall of fame on the website.` }),
  ];
  body.replaceChildren(...parts.filter((x): x is HTMLElement => x !== null));
}

const screen: ScreenDef = {
  title: "Credits",
  chrome: "full",

  render(ctx) {
    const body = h("div", { class: "credits-body" }, skeleton([60, 40, 50]));
    void api
      .getCredits()
      .then((c) => {
        if (!ctx.signal.aborted) render(body, c);
      })
      .catch((e: unknown) => {
        if (!ctx.signal.aborted) body.replaceChildren(h("p", { class: "fine", text: `The credits couldn't be loaded: ${errorText(e)}` }));
      });

    const join = h(
      "section",
      { class: "card credits-join", "aria-labelledby": "credits-join-title" },
      h("h2", { class: "card-title", id: "credits-join-title", text: "Your name could be here: contribute" }),
      h("p", { class: "fine", text: "A merged pull request, or a Co-authored-by line on one, puts your name in the release notes, the hall of fame and this screen." }),
      h(
        "div",
        { class: "row" },
        button("Contribute with your AI agent", { kind: "primary", icon: "external", primary: true, onClick: () => open(AI_QUICKSTART_URL) }),
        button("CONTRIBUTING.md", { kind: "secondary", icon: "external", onClick: () => open(CONTRIBUTING_URL) }),
        button("Hall of fame", { kind: "ghost", icon: "external", onClick: () => open(HALL_URL) }),
        button("Discord", { kind: "ghost", icon: "chat", onClick: () => open(DISCORD_URL) }),
      ),
    );

    return h(
      "div",
      { class: "about" },
      h(
        "header",
        { class: "screen-head" },
        h("p", { class: "eyebrow", text: "Credits" }),
        h("h1", { text: "Made by everyone listed here" }),
        h("p", { class: "lede", text: "Guarana is open source, a fork of Lichii and the yuzu, Eden and Eglantine line. These are the people who changed Guarana itself." }),
      ),
      body,
      join,
    );
  },
};

export default screen;
