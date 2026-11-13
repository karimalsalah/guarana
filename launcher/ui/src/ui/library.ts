// The game list under the hero. Loaded lazily when it scrolls near the viewport.

import { h } from "../dom";
import { bytes } from "../format";
import { icon } from "../icons";
import type { Game } from "../types";
import { button } from "./controls";

const CONTAINER: Record<Game["container"], string> = { nup: "NUP", nsp: "NSP", xci: "XCI", nx: "NX", other: "Game file" };

export function libraryList(games: Game[], opts: { canPlay: boolean; onPlay: (g: Game) => void }): HTMLElement {
  return h(
    "ul",
    { class: "games" },
    games.map((g, i) =>
      h(
        "li",
        { class: "game-row", style: { "--i": String(i) } },
        h("span", { class: "item-icon" }, icon("game")),
        h(
          "div",
          { class: "game-text" },
          h("p", { class: "item-title", text: g.title }),
          h("p", { class: "item-sub", text: `${CONTAINER[g.container]} · ${bytes(g.size_bytes)}` }),
        ),
        button("Play", {
          kind: "secondary",
          size: "sm",
          icon: "play",
          attrs: { "aria-label": `Play ${g.title}`, disabled: opts.canPlay ? undefined : true },
          onClick: () => opts.onPlay(g),
        }),
      ),
    ),
  );
}
