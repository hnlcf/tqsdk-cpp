import { normalizeIndicatorConfig, type IndicatorConfig } from "../indicators/indicators";

export type Theme = "dark" | "light";
export type View = "terminal" | "futures" | "options" | "ops";

const KEY_THEME = "tqsdk.theme";
const KEY_WATCHLIST = "tqsdk.watchlist.v1";
const KEY_SELECTED = "tqsdk.selected.v1";
const KEY_PERIOD = "tqsdk.period.v1";
const KEY_CHART_MODE = "tqsdk.chartMode.v1";
const KEY_INDICATORS = "tqsdk.indicators.v1";

export const DEFAULT_WATCHLIST = [
  "SHFE.rb2701",
  "SHFE.ru2701",
  "DCE.m2701",
  "CZCE.SA701",
  "DCE.jm2701",
];

function readJson<T>(key: string, fallback: T): T {
  try {
    const raw = window.localStorage.getItem(key);
    return raw ? (JSON.parse(raw) as T) : fallback;
  } catch {
    return fallback;
  }
}

function writeJson(key: string, value: unknown): void {
  try {
    window.localStorage.setItem(key, JSON.stringify(value));
  } catch {
    // private mode / quota: the current session still works.
  }
}

export function readTheme(): Theme {
  try {
    const query = new URLSearchParams(window.location.search).get("theme");
    if (query === "dark" || query === "light") return query;
    const saved = window.localStorage.getItem(KEY_THEME);
    if (saved === "dark" || saved === "light") return saved;
  } catch {
    // ignore
  }
  return window.matchMedia?.("(prefers-color-scheme: light)").matches ? "light" : "dark";
}

export function saveTheme(theme: Theme): void {
  try {
    window.localStorage.setItem(KEY_THEME, theme);
  } catch {
    // ignore
  }
}

export function readWatchlist(): string[] {
  try {
    const raw = window.localStorage.getItem(KEY_WATCHLIST);
    if (raw !== null) {
      const saved = JSON.parse(raw) as unknown;
      if (Array.isArray(saved)) {
        return [...new Set(saved.filter((item): item is string => typeof item === "string"))];
      }
    }
  } catch {
    // fall through to defaults
  }
  return [...DEFAULT_WATCHLIST];
}

export function saveWatchlist(symbols: string[]): void {
  writeJson(KEY_WATCHLIST, [...new Set(symbols.filter(Boolean))]);
}

export interface WatchlistGroup {
  id: string;
  name: string;
  symbols: string[];
}

const KEY_WATCHLISTS = "tqsdk.watchlists.v2";
const KEY_ACTIVE_WATCHLIST = "tqsdk.watchlists.active.v2";

function newListId(): string {
  if (typeof crypto !== "undefined" && "randomUUID" in crypto) {
    return crypto.randomUUID();
  }
  return `list-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
}

function sanitizeGroup(value: unknown): WatchlistGroup | null {
  if (!value || typeof value !== "object") return null;
  const row = value as Record<string, unknown>;
  if (typeof row.id !== "string" || typeof row.name !== "string") return null;
  const symbols = Array.isArray(row.symbols)
    ? [...new Set(row.symbols.filter((item): item is string => typeof item === "string"))]
    : [];
  return { id: row.id, name: row.name, symbols };
}

/**
 * Watchlists are versioned independently from the old single-list key.
 * Existing v1 arrays are migrated into one "默认" group.
 */
export function readWatchlists(): { groups: WatchlistGroup[]; activeId: string } {
  try {
    const raw = window.localStorage.getItem(KEY_WATCHLISTS);
    if (raw) {
      const parsed = JSON.parse(raw) as unknown;
      if (Array.isArray(parsed)) {
        const groups = parsed
          .map(sanitizeGroup)
          .filter((group): group is WatchlistGroup => group !== null);
        if (groups.length > 0) {
          const storedActive = window.localStorage.getItem(KEY_ACTIVE_WATCHLIST);
          const activeId = groups.some((group) => group.id === storedActive)
            ? (storedActive as string)
            : groups[0].id;
          return { groups, activeId };
        }
      }
    }
  } catch {
    // migrate/fallback below
  }
  const migrated = readWatchlist();
  const first: WatchlistGroup = { id: "default", name: "默认", symbols: migrated };
  return { groups: [first], activeId: first.id };
}

export function saveWatchlists(groups: WatchlistGroup[], activeId: string): void {
  const normalized = groups
    .map((group) => ({
      ...group,
      name: group.name.trim() || "未命名",
      symbols: [...new Set(group.symbols.filter(Boolean))],
    }))
    .filter((group) => group.id);
  writeJson(KEY_WATCHLISTS, normalized);
  try {
    window.localStorage.setItem(KEY_ACTIVE_WATCHLIST, activeId);
  } catch {
    // ignore
  }
}

export function createWatchlistGroup(name: string, symbols: string[] = []): WatchlistGroup {
  return { id: newListId(), name: name.trim() || "新自选", symbols: [...new Set(symbols)] };
}

export function readSelected(): string {
  try {
    return window.localStorage.getItem(KEY_SELECTED) || DEFAULT_WATCHLIST[0];
  } catch {
    return DEFAULT_WATCHLIST[0];
  }
}

export function saveSelected(symbol: string): void {
  try {
    window.localStorage.setItem(KEY_SELECTED, symbol);
  } catch {
    // ignore
  }
}

export function readPeriod(): string {
  try {
    return window.localStorage.getItem(KEY_PERIOD) || "1m";
  } catch {
    return "1m";
  }
}

export function savePeriod(period: string): void {
  try {
    window.localStorage.setItem(KEY_PERIOD, period);
  } catch {
    // ignore
  }
}

export function readChartMode(): "kline" | "time" {
  try {
    const query = new URLSearchParams(window.location.search).get("mode");
    if (query === "time" || query === "kline") return query;
    return window.localStorage.getItem(KEY_CHART_MODE) === "time" ? "time" : "kline";
  } catch {
    return "kline";
  }
}

export function saveChartMode(mode: "kline" | "time"): void {
  try {
    window.localStorage.setItem(KEY_CHART_MODE, mode);
  } catch {
    // ignore
  }
}

export function readIndicators(fallback: IndicatorConfig): IndicatorConfig {
  try {
    const requested = new URLSearchParams(window.location.search).get("indicators");
    if (requested !== null) {
      const keys = new Set(requested.split(",").map((item) => item.trim()).filter(Boolean));
      return normalizeIndicatorConfig({
        ...fallback,
        maPeriods: keys.has("ma") ? [5, 10, 20] : [],
        boll: keys.has("boll"),
        macd: keys.has("macd"),
        rsi: keys.has("rsi"),
      });
    }
  } catch {
    // fall back to persisted settings
  }
  return normalizeIndicatorConfig(readJson<Partial<IndicatorConfig>>(KEY_INDICATORS, fallback));
}

export function saveIndicators(config: IndicatorConfig): void {
  writeJson(KEY_INDICATORS, config);
}
