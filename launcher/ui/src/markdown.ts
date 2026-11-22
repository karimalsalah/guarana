// Minimal Markdown for release notes and report summaries. It builds DOM nodes and puts all text
// in text nodes, so HTML in the source is shown as text, never parsed. Links open through open_url
// (https only); anything else stays plain text.
//
// Supported: headings, paragraphs, - * + and 1. lists, > quotes, ``` fences, ---, **bold**,
// *italic*, `code`, [text](https://...), bare https:// URLs.

import { api } from "./api";

type InlineRule = { re: RegExp; make: (m: RegExpExecArray) => Node };

function link(text: string, href: string): Node {
  if (!/^https:\/\/[^\s]+$/i.test(href)) return document.createTextNode(text);
  const a = document.createElement("a");
  a.href = href;
  a.textContent = text;
  a.rel = "noreferrer";
  a.addEventListener("click", (e) => {
    e.preventDefault();
    void api.openUrl(href).catch(() => undefined);
  });
  return a;
}

function wrap(tag: "strong" | "em" | "code", children: Node[] | string): Node {
  const el = document.createElement(tag);
  if (typeof children === "string") el.textContent = children;
  else el.append(...children);
  return el;
}

const RULES: InlineRule[] = [
  { re: /`([^`]+)`/, make: (m) => wrap("code", m[1]) },
  { re: /\[([^\]]+)\]\(([^)\s]+)\)/, make: (m) => link(m[1], m[2]) },
  { re: /\*\*([^*]+)\*\*|(?<![A-Za-z0-9])__([^_]+)__(?![A-Za-z0-9])/, make: (m) => wrap("strong", inline(m[1] ?? m[2] ?? "")) },
  { re: /\*([^*\s][^*]*)\*|(?<![A-Za-z0-9])_([^_\s][^_]*)_(?![A-Za-z0-9])/, make: (m) => wrap("em", inline(m[1] ?? m[2] ?? "")) },
  { re: /https:\/\/[^\s<>()]+[^\s<>().,;:!?'"]/, make: (m) => link(m[0], m[0]) },
];

export function inline(text: string): Node[] {
  const out: Node[] = [];
  let rest = text;
  while (rest) {
    let best: { index: number; m: RegExpExecArray; rule: InlineRule } | null = null;
    for (const rule of RULES) {
      const m = rule.re.exec(rest);
      if (m && (best === null || m.index < best.index)) best = { index: m.index, m, rule };
    }
    if (!best) {
      out.push(document.createTextNode(rest));
      break;
    }
    if (best.index > 0) out.push(document.createTextNode(rest.slice(0, best.index)));
    out.push(best.rule.make(best.m));
    rest = rest.slice(best.index + best.m[0].length);
  }
  return out;
}

/** Renders Markdown into a fragment. `headingBase` is the level a "#" maps to (default h3). */
export function renderMarkdown(source: string, headingBase = 3): DocumentFragment {
  const frag = document.createDocumentFragment();
  const lines = source.replace(/\r\n?/g, "\n").split("\n");
  let para: string[] = [];
  let list: HTMLElement | null = null;
  let quote: HTMLElement | null = null;

  // Line breaks inside a paragraph stay line breaks, as GitHub shows them in issues and releases.
  const flushPara = () => {
    if (!para.length) return;
    const p = document.createElement("p");
    para.forEach((line, i) => {
      if (i > 0) p.appendChild(document.createElement("br"));
      p.append(...inline(line));
    });
    (quote ?? frag).appendChild(p);
    para = [];
  };
  const closeList = () => {
    list = null;
  };
  const closeQuote = () => {
    flushPara();
    quote = null;
  };

  for (let i = 0; i < lines.length; i++) {
    const raw = lines[i];
    const line = raw.trimEnd();

    if (/^\s*```/.test(line)) {
      flushPara();
      closeList();
      closeQuote();
      const code: string[] = [];
      i += 1;
      while (i < lines.length && !/^\s*```/.test(lines[i])) {
        code.push(lines[i]);
        i += 1;
      }
      const pre = document.createElement("pre");
      const c = document.createElement("code");
      c.textContent = code.join("\n");
      pre.appendChild(c);
      frag.appendChild(pre);
      continue;
    }

    if (!line.trim()) {
      flushPara();
      closeList();
      closeQuote();
      continue;
    }

    const heading = /^(#{1,6})\s+(.*)$/.exec(line);
    if (heading) {
      flushPara();
      closeList();
      closeQuote();
      const level = Math.min(6, headingBase + heading[1].length - 1);
      const el = document.createElement(`h${level}`);
      el.append(...inline(heading[2]));
      frag.appendChild(el);
      continue;
    }

    if (/^\s*([-*_])(\s*\1){2,}\s*$/.test(line)) {
      flushPara();
      closeList();
      closeQuote();
      frag.appendChild(document.createElement("hr"));
      continue;
    }

    const q = /^\s*>\s?(.*)$/.exec(line);
    if (q) {
      closeList();
      if (!quote) {
        flushPara();
        quote = document.createElement("blockquote");
        frag.appendChild(quote);
      }
      if (q[1].trim()) para.push(q[1].trim());
      else flushPara();
      continue;
    } else if (quote) {
      closeQuote();
    }

    const ul = /^\s*[-*+]\s+(.*)$/.exec(line);
    const ol = /^\s*\d+[.)]\s+(.*)$/.exec(line);
    if (ul || ol) {
      flushPara();
      const tag = ul ? "UL" : "OL";
      if (!list || list.tagName !== tag) {
        list = document.createElement(ul ? "ul" : "ol");
        frag.appendChild(list);
      }
      const li = document.createElement("li");
      li.append(...inline(((ul ?? ol) as RegExpExecArray)[1]));
      list.appendChild(li);
      continue;
    }

    if (list && /^\s{2,}\S/.test(raw)) {
      const last = list.lastElementChild;
      if (last) {
        last.append(" ", ...inline(line.trim()));
        continue;
      }
    }

    closeList();
    para.push(line.trim());
  }
  flushPara();
  return frag;
}
