// The game list under the hero. Loaded lazily when it scrolls near the viewport.

import { h } from "../dom";
import { bytes } from "../format";
import { icon } from "../icons";
import type { Game } from "../types";
import { button } from "./controls";

const CONTAINER: Record<Game["container"], string> = { nup: "NUP", nsp: "NSP", xci: "XCI", nx: "NX", other: "Game file" };

/** onPlay gets the row's title element, so the launch stage can morph from it. */
export function libraryList(games: Game[], opts: { canPlay: boolean; onPlay: (g: Game, title: HTMLElement) => void }): HTMLElement {
  return h(
    "ul",
    { class: "games" },
    games.map((g, i) => {
      const title = h("p", { class: "item-title game-title", text: g.title });
      // A typographic row, not a tile: its number, the title, the file, and Play.
      return h(
        "li",
        { class: "game-row", style: { "--i": String(Math.min(i, 10)) } },
        h("span", { class: "game-index", "aria-hidden": "true" }, icon("game"), String(i + 1).padStart(2, "0")),
        h("div", { class: "game-text" }, title, h("p", { class: "item-sub", text: `${CONTAINER[g.container]} · ${bytes(g.size_bytes)}` })),
        button("Play", {
          kind: "secondary",
          size: "sm",
          icon: "play",
          attrs: { "aria-label": `Play ${g.title}`, disabled: opts.canPlay ? undefined : true },
          onClick: () => opts.onPlay(g, title),
        }),
      );
    }),
  );
}
