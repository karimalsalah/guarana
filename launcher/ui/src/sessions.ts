// Last-played times. CONTRACT.md has no persisted play history, so the UI keeps its own from
// game://exited. It only knows sessions started from the launcher.

interface SessionRecord {
  last: string;
  seconds: number;
  total: number;
}

const KEY = "guarana.sessions";

function readAll(): Record<string, SessionRecord> {
  try {
    const raw = localStorage.getItem(KEY);
    const parsed: unknown = raw ? JSON.parse(raw) : {};
    return parsed && typeof parsed === "object" ? (parsed as Record<string, SessionRecord>) : {};
  } catch {
    return {};
  }
}

export function lastSession(titleId: string): SessionRecord | null {
  const r = readAll()[titleId];
  return r && typeof r.last === "string" ? r : null;
}

export function recordSession(titleId: string, seconds: number): void {
  const all = readAll();
  const prev = all[titleId];
  all[titleId] = {
    last: new Date().toISOString(),
    seconds: Math.max(0, Math.round(seconds)),
    total: (prev?.total ?? 0) + Math.max(0, Math.round(seconds)),
  };
  try {
    localStorage.setItem(KEY, JSON.stringify(all));
  } catch {
    /* storage full or disabled: the hero just shows no last-played line */
  }
}
