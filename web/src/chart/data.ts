import type { KlineBar } from "../api/client";
import type { OHLCV } from "../indicators/indicators";

/** Convert API klines (ns timestamps) to chart seconds, dropping invalid rows. */
export function toOHLCV(rows: KlineBar[]): OHLCV[] {
  return rows
    .filter((row) => Number.isFinite(row.datetime) && row.datetime > 0)
    .map((row) => ({
      time: Math.floor(row.datetime / 1_000_000_000),
      open: row.open,
      high: row.high,
      low: row.low,
      close: row.close,
      volume: row.volume,
    }))
    .sort((a, b) => a.time - b.time);
}

/** Merge incremental/live bars by time and keep the recent window bounded. */
export function mergeBars(previous: OHLCV[], incoming: OHLCV[]): OHLCV[] {
  const map = new Map<number, OHLCV>();
  for (const bar of previous) map.set(bar.time, bar);
  for (const bar of incoming) map.set(bar.time, bar);
  return [...map.values()].sort((a, b) => a.time - b.time).slice(-2000);
}
