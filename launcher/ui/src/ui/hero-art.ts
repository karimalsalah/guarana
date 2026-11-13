// Brand-abstract key art for the game hero and the launch stage: the drop, its speed streaks and
// echo arcs (predicted frames) on a night gradient. Nothing in it depicts a game.
// The echo arcs are separate SVG layers so their pulse runs on the compositor (transform/opacity).

import { h, nextId, svg } from "../dom";
import { DROP_D, echoD, LEAF_D } from "../icons";

const VIEW = "-120 0 210 64";

export function heroArt(opts: { cls?: string } = {}): HTMLElement {
  const id = nextId("art");
  const mark = svg(
    `<svg class="art-svg" viewBox="${VIEW}" preserveAspectRatio="xMaxYMid meet" aria-hidden="true" focusable="false">
      <defs>
        <linearGradient id="${id}-f" x1="22" y1="19" x2="48" y2="53" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="#FFC94A"/><stop offset="1" stop-color="#F2851A"/></linearGradient>
        <linearGradient id="${id}-s" x1="-118" y1="0" x2="20" y2="0" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="#FFC94A" stop-opacity="0"/><stop offset=".7" stop-color="#FFC94A" stop-opacity=".28"/><stop offset="1" stop-color="#FFC94A" stop-opacity=".85"/></linearGradient>
        <mask id="${id}-m" maskUnits="userSpaceOnUse" x="-130" y="-8" width="240" height="80"><rect x="-130" y="-8" width="240" height="80" fill="#fff"/><path d="M-4 29.2H29M-4 42.8H29" stroke="#000" stroke-width="3.2" stroke-linecap="round"/></mask>
      </defs>
      <g fill="none" stroke="url(#${id}-s)" stroke-linecap="round">
        <path d="M-114 29.2H14" stroke-width="1.7"/>
        <path d="M-80 36H4" stroke-width=".9"/>
        <path d="M-98 42.8H14" stroke-width="1.7"/>
      </g>
      <path d="${DROP_D}" fill="url(#${id}-f)" mask="url(#${id}-m)"/>
      <path d="${LEAF_D}" fill="#3CCF7E"/>
    </svg>`,
  );
  const echoes = [0, 1, 2].map((i) =>
    svg(
      `<svg class="art-echo art-echo-${i}" viewBox="${VIEW}" preserveAspectRatio="xMaxYMid meet" aria-hidden="true" focusable="false"><path d="${echoD(i * 7)}" fill="none" stroke="#F59E0B" stroke-width="3" stroke-linecap="round"/></svg>`,
    ),
  );
  return h(
    "div",
    { class: `art ${opts.cls ?? ""}`, "aria-hidden": "true" },
    h("div", { class: "art-sky" }),
    h("div", { class: "art-dust" }),
    h("div", { class: "art-glow ambient" }),
    h("div", { class: "art-mark" }, mark, h("div", { class: "art-echoes ambient" }, echoes)),
    h("div", { class: "art-scrim" }),
  );
}
