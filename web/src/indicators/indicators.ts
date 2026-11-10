export interface OHLCV {
  time: number;
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
}

export interface ValuePoint {
  time: number;
  value: number;
}

/**
 * Select the bars for an intraday chart. Futures night sessions start on the
 * previous calendar day, so include bars from 20:00 of the prior day when the
 * latest bar is a morning/afternoon bar.
 */
/**
 * Session boundaries are China Standard Time regardless of the browser
 * timezone. Lightweight Charts stores seconds as UTC, so convert the China
 * wall clock to UTC seconds explicitly.
 */
function chinaSessionStart(latest: number): number {
  const chinaMs = (latest + 8 * 3600) * 1000;
  const china = new Date(chinaMs);
  let year = china.getUTCFullYear();
  let month = china.getUTCMonth();
  let day = china.getUTCDate();
  const hour = china.getUTCHours();
  // 20:00 CST is the cutoff for the next night session; the current
  // night session belongs to today. Morning/afternoon bars belong to the
  // previous night session.
  if (hour < 20) day -= 1;
  const startUtc = Date.UTC(year, month, day, 20, 0, 0) - 8 * 3600 * 1000;
  if (hour >= 20) {
    // Date.UTC already points at today's 20:00, not yesterday's.
    return Math.floor((Date.UTC(china.getUTCFullYear(), china.getUTCMonth(), china.getUTCDate(), 20, 0, 0) - 8 * 3600 * 1000) / 1000);
  }
  return Math.floor(startUtc / 1000);
}

export function intradayBars(bars: OHLCV[], maxBars = 600): OHLCV[] {
  if (bars.length === 0) return [];
  const latest = bars[bars.length - 1].time;
  const startSeconds = chinaSessionStart(latest);
  let filtered = bars.filter((bar) => bar.time >= startSeconds);
  if (filtered.length === 0) {
    // No night session: use the latest China calendar day's bars only.
    const chinaMs = (latest + 8 * 3600) * 1000;
    const china = new Date(chinaMs);
    const dayStart =
      Date.UTC(china.getUTCFullYear(), china.getUTCMonth(), china.getUTCDate(), 0, 0, 0) -
      8 * 3600 * 1000;
    filtered = bars.filter((bar) => bar.time * 1000 >= dayStart);
  }
  if (filtered.length === 0) return bars.slice(-maxBars);
  return filtered.slice(-maxBars);
}

/** Format a UTC-second timestamp as China wall-clock HH:mm. */
export function chinaTimeLabel(seconds: number): string {
  const china = new Date((seconds + 8 * 3600) * 1000);
  return `${String(china.getUTCHours()).padStart(2, "0")}:${String(
    china.getUTCMinutes(),
  ).padStart(2, "0")}`;
}

/** Cumulative volume-weighted typical price, used by the intraday average line. */
export function averagePriceLine(bars: OHLCV[]): ValuePoint[] {
  let tradedValue = 0;
  let tradedVolume = 0;
  const out: ValuePoint[] = [];
  for (const bar of bars) {
    const typical = (bar.high + bar.low + bar.close) / 3;
    const volume = Math.max(0, bar.volume);
    tradedValue += typical * volume;
    tradedVolume += volume;
    out.push({
      time: bar.time,
      value: tradedVolume > 0 ? tradedValue / tradedVolume : bar.close,
    });
  }
  return out;
}
