// Tiny DOM helpers. Text always goes through text nodes, so data from the backend can't inject markup.

export type Child = Node | string | number | false | null | undefined | Child[];
export type Attrs = Record<string, unknown>;

export function h<K extends keyof HTMLElementTagNameMap>(tag: K, attrs?: Attrs | null, ...children: Child[]): HTMLElementTagNameMap[K] {
  const el = document.createElement(tag);
  if (attrs) setAttrs(el, attrs);
  append(el, children);
  return el;
}

export function setAttrs(el: HTMLElement, attrs: Attrs): void {
  for (const [key, value] of Object.entries(attrs)) {
    if (value === undefined || value === null || value === false) continue;
    if (key === "class") {
      el.className = String(value);
    } else if (key === "text") {
      el.textContent = String(value);
    } else if (key === "style" && typeof value === "object") {
      for (const [prop, v] of Object.entries(value as Record<string, unknown>)) {
        if (v === undefined || v === null) continue;
        if (prop.startsWith("--") || prop.includes("-")) el.style.setProperty(prop, String(v));
        else el.style.setProperty(prop.replace(/[A-Z]/g, (m) => "-" + m.toLowerCase()), String(v));
      }
    } else if (key === "dataset" && typeof value === "object") {
      for (const [k, v] of Object.entries(value as Record<string, unknown>)) {
        if (v !== undefined && v !== null && v !== false) el.dataset[k] = String(v);
      }
    } else if (key.startsWith("on") && typeof value === "function") {
      el.addEventListener(key.slice(2).toLowerCase(), value as EventListener);
    } else if (value === true) {
      el.setAttribute(key, "");
    } else {
      el.setAttribute(key, String(value));
    }
  }
}

export function append(parent: Node, children: Child[]): void {
  for (const c of children) {
    if (c === null || c === undefined || c === false) continue;
    if (Array.isArray(c)) append(parent, c);
    else if (typeof c === "string" || typeof c === "number") parent.appendChild(document.createTextNode(String(c)));
    else parent.appendChild(c);
  }
}

/** Parses an SVG string the UI itself authored (icons, art). Never pass backend data here. */
export function svg(markup: string): SVGSVGElement {
  const t = document.createElement("template");
  t.innerHTML = markup.trim();
  const el = t.content.firstElementChild;
  if (!el || el.tagName.toLowerCase() !== "svg") throw new Error("svg(): markup is not an <svg>");
  return el as SVGSVGElement;
}

export function $<T extends Element = HTMLElement>(selector: string, root: ParentNode = document): T | null {
  return root.querySelector<T>(selector);
}

export function $$<T extends Element = HTMLElement>(selector: string, root: ParentNode = document): T[] {
  return Array.from(root.querySelectorAll<T>(selector));
}

/** Replaces an element's children. */
export function fill(el: Element, ...children: Child[]): void {
  el.replaceChildren();
  append(el, children);
}

let uid = 0;
export function nextId(prefix = "g"): string {
  uid += 1;
  return `${prefix}-${uid}`;
}
