// Inline SVG icons (24 px grid, stroke = currentColor) and the brand mark. All original artwork.

import { nextId, svg } from "./dom";

const P: Record<string, string> = {
  check: '<path d="M5 12.5l4.2 4.2L19 7"/>',
  ok: '<circle cx="12" cy="12" r="8.8"/><path d="M8.2 12.3l2.6 2.6 5-5.4"/>',
  warn: '<path d="M12 4.2 3.2 19.3h17.6z"/><path d="M12 10v4.4M12 17.1v.1"/>',
  fail: '<circle cx="12" cy="12" r="8.8"/><path d="M9.2 9.2l5.6 5.6M14.8 9.2l-5.6 5.6"/>',
  info: '<circle cx="12" cy="12" r="8.8"/><path d="M12 11v5.4M12 7.9v.1"/>',
  play: '<path d="M8.6 5.4c0-.8.9-1.3 1.6-.9l8.8 6.1c.6.4.6 1.4 0 1.8l-8.8 6.1c-.7.4-1.6-.1-1.6-.9z" fill="currentColor" stroke="none"/>',
  chevronRight: '<path d="M9.5 6l6 6-6 6"/>',
  chevronLeft: '<path d="M14.5 6l-6 6 6 6"/>',
  chevronDown: '<path d="M6 9.5l6 6 6-6"/>',
  arrowRight: '<path d="M4.5 12h15M13.5 6l6 6-6 6"/>',
  refresh: '<path d="M19.5 12a7.5 7.5 0 1 1-2.2-5.3"/><path d="M19.6 4.4v4.2h-4.2"/>',
  copy: '<rect x="8.5" y="8.5" width="11" height="11" rx="2.2"/><path d="M15.5 8.5V6.2a1.7 1.7 0 0 0-1.7-1.7H6.2a1.7 1.7 0 0 0-1.7 1.7v7.6a1.7 1.7 0 0 0 1.7 1.7h2.3"/>',
  external: '<path d="M13.5 4.5h6v6M19.5 4.5l-8 8"/><path d="M17.5 13.5v4.3a1.7 1.7 0 0 1-1.7 1.7H6.2a1.7 1.7 0 0 1-1.7-1.7V8.2a1.7 1.7 0 0 1 1.7-1.7h4.3"/>',
  folder: '<path d="M3.8 7.2a1.7 1.7 0 0 1 1.7-1.7h4l2 2.2h7a1.7 1.7 0 0 1 1.7 1.7v8.4a1.7 1.7 0 0 1-1.7 1.7H5.5a1.7 1.7 0 0 1-1.7-1.7z"/>',
  key: '<circle cx="8" cy="15.5" r="3.6"/><path d="M10.6 12.9 19 4.5M16 7.5l2.4 2.4M13.8 9.7l1.8 1.8"/>',
  chip: '<rect x="6.5" y="6.5" width="11" height="11" rx="2"/><path d="M9.5 3.5v3M14.5 3.5v3M9.5 17.5v3M14.5 17.5v3M3.5 9.5h3M3.5 14.5h3M17.5 9.5h3M17.5 14.5h3"/>',
  game: '<rect x="3.8" y="5.5" width="16.4" height="13" rx="2.6"/><path d="M10.4 9.6v4.8l4.1-2.4z"/>',
  gamepad: '<path d="M7.6 8.5h8.8a4.6 4.6 0 0 1 4.5 5.5l-.6 3a2.2 2.2 0 0 1-3.8 1l-2-2.4H9.5l-2 2.4a2.2 2.2 0 0 1-3.8-1l-.6-3a4.6 4.6 0 0 1 4.5-5.5z"/><path d="M8.6 11.3v3M7.1 12.8h3"/><path d="M15.5 12.1v.1M17.2 13.7v.1"/>',
  sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2.8v2M12 19.2v2M2.8 12h2M19.2 12h2M5.5 5.5l1.4 1.4M17.1 17.1l1.4 1.4M5.5 18.5l1.4-1.4M17.1 6.9l1.4-1.4"/>',
  moon: '<path d="M19.6 14.6A7.6 7.6 0 0 1 9.4 4.4a7.6 7.6 0 1 0 10.2 10.2z"/>',
  home: '<path d="M4.5 10.6 12 4.5l7.5 6.1V19a1 1 0 0 1-1 1h-4.1v-5.5H9.6V20H5.5a1 1 0 0 1-1-1z"/>',
  sliders: '<path d="M4.5 7h9.5M18 7h1.5M4.5 17h3.5M12 17h7.5"/><circle cx="16" cy="7" r="2"/><circle cx="10" cy="17" r="2"/>',
  download: '<path d="M12 4v10.5m0 0-4.2-4.2m4.2 4.2 4.2-4.2M5 19.5h14"/>',
  pulse: '<path d="M3.5 12.5h4l2.2-5.5 4.3 10.5 2.2-5h4.3"/>',
  flag: '<path d="M5.5 20.5V4.5M5.5 5h11.2l-2.1 4 2.1 4H5.5"/>',
  bug: '<rect x="7.5" y="8" width="9" height="11.5" rx="4.5"/><path d="M12 8v11.5M9.6 5.4l1 2.6M14.4 5.4l-1 2.6M4.5 12.5h3M16.5 12.5h3M5 17.4l2.6-1.3M19 17.4l-2.6-1.3M5 8l2.6 1.4M19 8l-2.6 1.4"/>',
  gauge: '<path d="M4.4 16.5a7.6 7.6 0 1 1 15.2 0"/><path d="M12 16.5l3.6-4.6"/><circle cx="12" cy="16.5" r="1.1"/>',
  bolt: '<path d="M13.2 3.5 6 13.2h5.3L10.6 20.5l7.4-9.9h-5.3z"/>',
  shield: '<path d="M12 3.5l7 3v5.2c0 4.3-2.9 7.8-7 9.8-4.1-2-7-5.5-7-9.8V6.5z"/><path d="M8.8 12.2l2.2 2.2 4.2-4.4"/>',
  search: '<circle cx="11" cy="11" r="6.5"/><path d="M16 16l4.5 4.5"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  minus: '<path d="M5 12h14"/>',
  close: '<path d="M6.5 6.5l11 11M17.5 6.5l-11 11"/>',
  history: '<path d="M4.5 12a7.5 7.5 0 1 0 2.2-5.3"/><path d="M4.4 4.4v4.2h4.2"/><path d="M12 8v4.5l3 1.8"/>',
  sparkle: '<path d="M12 3.5l1.6 4.6 4.6 1.6-4.6 1.6L12 16l-1.6-4.7-4.6-1.6 4.6-1.6z"/><path d="M18.5 15.5l.7 1.8 1.8.7-1.8.7-.7 1.8-.7-1.8-1.8-.7 1.8-.7z"/>',
  flask: '<path d="M9.5 3.5h5M10.5 3.5v5.2L5.6 17.4a2 2 0 0 0 1.8 3.1h9.2a2 2 0 0 0 1.8-3.1l-4.9-8.7V3.5"/><path d="M7.7 14.5h8.6"/>',
  leaf: '<path d="M5 19c0-8 5.5-13.5 14-14 0 8.5-6 14-14 14z"/><path d="M5 19l8-8"/>',
  display: '<rect x="3.5" y="4.5" width="17" height="11.5" rx="2"/><path d="M9 20h6M12 16v4"/>',
  memory: '<rect x="3.5" y="7" width="17" height="10" rx="1.6"/><path d="M7 10.5v3M10.5 10.5v3M14 10.5v3M17.5 10.5v3M6.5 17v2M17.5 17v2"/>',
  plug: '<path d="M9 3.5v4M15 3.5v4M7 7.5h10v3a5 5 0 0 1-10 0z"/><path d="M12 15.5v5"/>',
  laptop: '<rect x="5" y="5" width="14" height="10" rx="1.6"/><path d="M3 18.5h18"/>',
  chat: '<path d="M4.5 5.5h15v10h-8.2l-4 3.4v-3.4H4.5z"/>',
  code: '<path d="M8.5 7.5 4 12l4.5 4.5M15.5 7.5 20 12l-4.5 4.5"/>',
  clock: '<circle cx="12" cy="12" r="8.5"/><path d="M12 7.5V12l3 2"/>',
};

export type IconName = keyof typeof P;

export function icon(name: IconName, cls = ""): SVGSVGElement {
  const body = P[name] ?? "";
  return svg(
    `<svg class="icon${cls ? " " + cls : ""}" viewBox="0 0 24 24" width="24" height="24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" focusable="false">${body}</svg>`,
  );
}

/** The drop: circle (centre 36,36, r 17) with tangents meeting at the tail tip on y = 36. */
export function dropPath(tipX: number): string {
  const cx = 36;
  const cy = 36;
  const r = 17;
  const d = Math.max(r + 0.05, cx - tipX);
  const cos = r / d;
  const sin = Math.sqrt(1 - cos * cos);
  const x = cx - r * cos;
  const yTop = cy - r * sin;
  const yBot = cy + r * sin;
  const f = (n: number) => Number(n.toFixed(2));
  return `M${f(cx - d)} ${cy}L${f(x)} ${f(yTop)}A${r} ${r} 0 1 1 ${f(x)} ${f(yBot)}Z`;
}

export const DROP_D = "M4 36L26.97 21.6A17 17 0 1 1 26.97 50.4Z";
export const LEAF_D = "M37.5 20Q38.5 10.5 48 9Q47.5 18.5 37.5 20Z";
export const STREAKS_D = "M29 29.2H-4M29 42.8H-4";
export const echoD = (dx: number) => `M${53.93 + dx} 22.98A17 17 0 0 1 ${53.93 + dx} 49.02`;

/** Brand mark with unique gradient/mask ids, so several can live on one page. */
export function logoMark(cls = "mark", echoes = 1): SVGSVGElement {
  const id = nextId("mk");
  const opacity = [0.6, 0.3, 0.14];
  const arcs = [0, 7, 14]
    .slice(0, Math.max(0, Math.min(3, echoes)))
    .map((dx, i) => `<path class="mark-echo mark-echo-${i}" d="${echoD(dx)}" fill="none" stroke="#F59E0B" stroke-opacity="${opacity[i]}" stroke-width="3" stroke-linecap="round"/>`)
    .join("");
  const width = echoes > 1 ? 64 + (Math.min(3, echoes) - 1) * 7 : 64;
  return svg(
    `<svg class="${cls}" viewBox="0 0 ${width} 64" aria-hidden="true" focusable="false">
      <defs>
        <linearGradient id="${id}-f" x1="22" y1="19" x2="48" y2="53" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="#FFC94A"/><stop offset="1" stop-color="#F2851A"/></linearGradient>
        <mask id="${id}-m" maskUnits="userSpaceOnUse" x="-8" y="-8" width="100" height="80"><rect x="-8" y="-8" width="100" height="80" fill="#fff"/><path d="M-4 29.2H29M-4 42.8H29" stroke="#000" stroke-width="3.2" stroke-linecap="round"/></mask>
      </defs>
      <path class="mark-drop" d="${DROP_D}" fill="url(#${id}-f)" mask="url(#${id}-m)"/>
      <path class="mark-leaf" d="${LEAF_D}" fill="#2FBF71"/>
      ${arcs}
    </svg>`,
  );
}
