// Dark by default; light is an explicit choice, remembered per user.

import { viewTransition } from "./motion";

export type Theme = "dark" | "light";

const KEY = "guarana.theme";
const root = document.documentElement;
const subs = new Set<(t: Theme) => void>();

export function currentTheme(): Theme {
  return root.dataset.theme === "light" ? "light" : "dark";
}

export function initTheme(): void {
  let saved: string | null = null;
  try {
    saved = localStorage.getItem(KEY);
  } catch {
    saved = null;
  }
  apply(saved === "light" ? "light" : "dark");
}

function apply(t: Theme): void {
  root.dataset.theme = t;
  document.querySelector('meta[name="color-scheme"]')?.setAttribute("content", t === "light" ? "light dark" : "dark light");
  document.querySelector('meta[name="theme-color"]')?.setAttribute("content", t === "light" ? "#f5f0e6" : "#000000");
  for (const fn of subs) fn(t);
}

/** Switches theme with a circular reveal that starts at `origin` (the toggle button). */
export async function setTheme(t: Theme, origin?: { x: number; y: number }): Promise<void> {
  try {
    localStorage.setItem(KEY, t);
  } catch {
    /* private mode: the choice lasts for this session */
  }
  if (origin) {
    root.style.setProperty("--reveal-x", `${Math.round(origin.x)}px`);
    root.style.setProperty("--reveal-y", `${Math.round(origin.y)}px`);
  }
  await viewTransition(() => apply(t), "theme");
}

export function onThemeChange(fn: (t: Theme) => void): () => void {
  subs.add(fn);
  return () => subs.delete(fn);
}
