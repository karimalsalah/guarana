// Formatting helpers: sizes, durations, dates, paths, progress lines.

import type { Progress } from "./types";

export function bytes(n: number): string {
  if (!Number.isFinite(n) || n < 0) return "–";
  const units = ["bytes", "KB", "MB", "GB", "TB"];
  let v = n;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i += 1;
  }
  if (i === 0) return `${Math.round(v)} ${units[0]}`;
  return `${v >= 100 ? Math.round(v) : v.toFixed(1)} ${units[i]}`;
}

export function duration(seconds: number): string {
  const s = Math.max(0, Math.round(seconds));
  if (s < 60) return `${s} s`;
  const m = Math.round(s / 60);
  if (m < 60) return `${m} min`;
  const h = Math.floor(m / 60);
  const rest = m % 60;
  return rest ? `${h} h ${rest} min` : `${h} h`;
}

const rtf = new Intl.RelativeTimeFormat(undefined, { numeric: "auto" });

export function relativeTime(iso: string | number | Date): string {
  const t = new Date(iso).getTime();
  if (!Number.isFinite(t)) return "";
  const diff = (t - Date.now()) / 1000;
  const abs = Math.abs(diff);
  if (abs < 45) return "just now";
  if (abs < 3600) return rtf.format(Math.round(diff / 60), "minute");
  if (abs < 86_400) return rtf.format(Math.round(diff / 3600), "hour");
  if (abs < 86_400 * 30) return rtf.format(Math.round(diff / 86_400), "day");
  if (abs < 86_400 * 365) return rtf.format(Math.round(diff / (86_400 * 30)), "month");
  return rtf.format(Math.round(diff / (86_400 * 365)), "year");
}

const dateFmt = new Intl.DateTimeFormat(undefined, { month: "short", day: "numeric", year: "numeric" });

export function shortDate(iso: string): string {
  const d = new Date(iso);
  return Number.isFinite(d.getTime()) ? dateFmt.format(d) : "";
}

export function plural(n: number, one: string, many: string): string {
  return `${n.toLocaleString()} ${n === 1 ? one : many}`;
}

export function v(version: string | null | undefined): string {
  if (!version) return "–";
  return /^\d/.test(version) ? `v${version}` : version;
}

/** Shows C:\Users\<name>\… as ~\… on screen. Commands still get the real path. */
export function prettyPath(path: string): string {
  return path.replace(/^[A-Za-z]:\\Users\\[^\\]+/i, "~");
}

/** Removes user names from text that leaves the app (copied diagnostics). The backend scrubs reports too. */
export function scrub(text: string): string {
  return text
    .replace(/[A-Za-z]:\\Users\\[^\\\s"'<>|]+/gi, "%USERPROFILE%")
    .replace(/\/(?:Users|home)\/[^/\s"'<>|]+/g, "~");
}

export function dirname(path: string): string {
  const i = Math.max(path.lastIndexOf("\\"), path.lastIndexOf("/"));
  return i > 0 ? path.slice(0, i) : path;
}

export function basename(path: string): string {
  const i = Math.max(path.lastIndexOf("\\"), path.lastIndexOf("/"));
  return i >= 0 ? path.slice(i + 1) : path;
}

/** "Intel(R) Arc(TM) 140T GPU (16GB)" -> "Intel Arc 140T". */
export function gpuName(name: string): string {
  return name
    .replace(/\((R|TM|C)\)/gi, "")
    .replace(/\s+GPU\b/i, "")
    .replace(/\(\s*\d+\s*GB\s*\)/i, "")
    .replace(/\s{2,}/g, " ")
    .trim();
}

/** One line for an install://progress event: resolve, download (bytes), verify, extract (files), done. */
export function installText(p: Progress): string {
  switch (p.stage) {
    case "resolve":
      return "Finding the download…";
    case "download":
      return p.total > 0 ? `Downloading ${bytes(p.done)} of ${bytes(p.total)}` : "Downloading…";
    case "verify":
      return "Checking the SHA-256…";
    case "extract":
      return p.total > 0 ? `Unpacking ${p.done.toLocaleString()} of ${p.total.toLocaleString()} files` : "Unpacking…";
    case "done":
      return "Finishing…";
    default:
      return p.message ?? "Installing…";
  }
}

/** Windows exit codes read better in hex (0xC0000005); small ones stay decimal. */
export function exitCode(code: number): string {
  return code > 255 || code < 0 ? `0x${(code >>> 0).toString(16).toUpperCase().padStart(8, "0")}` : String(code);
}

export function clamp(n: number, lo: number, hi: number): number {
  return Math.min(hi, Math.max(lo, n));
}
