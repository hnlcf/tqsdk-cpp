import { describe, expect, it } from "vitest";
import type { KlineBar } from "../api/client";
import { mergeBars, toOHLCV } from "./data";

const bar = (datetime: number, close: number): KlineBar => ({
  datetime,
  open: close - 1,
  high: close + 1,
  low: close - 2,
  close,
  volume: 10,
});

describe("chart data", () => {
  it("converts ns timestamps, sorts and drops invalid rows", () => {
    const rows = toOHLCV([
      bar(2_000_000_000, 12),
      { ...bar(0, 99), datetime: 0 },
      bar(1_000_000_000, 10),
    ]);
    expect(rows.map((row) => row.time)).toEqual([1, 2]);
    expect(rows[0].close).toBe(10);
  });

  it("merges live bars by time and lets incoming values win", () => {
    const previous = toOHLCV([bar(1_000_000_000, 10), bar(2_000_000_000, 12)]);
    const incoming = toOHLCV([bar(2_000_000_000, 13), bar(3_000_000_000, 14)]);
    const merged = mergeBars(previous, incoming);
    expect(merged.map((row) => row.time)).toEqual([1, 2, 3]);
    expect(merged[1].close).toBe(13);
  });
});
