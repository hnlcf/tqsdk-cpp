import type { IndicatorConfig } from "../indicators/indicators";

export type Theme = "dark" | "light";
export type View = "terminal" | "futures" | "options";

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
      return {
        ...fallback,
        maPeriods: keys.has("ma") ? [5, 10, 20] : [],
        boll: keys.has("boll"),
        macd: keys.has("macd"),
        rsi: keys.has("rsi"),
      };
    }
  } catch {
    // fall back to persisted settings
  }
  return readJson<IndicatorConfig>(KEY_INDICATORS, fallback);
}

export function saveIndicators(config: IndicatorConfig): void {
  writeJson(KEY_INDICATORS, config);
}
