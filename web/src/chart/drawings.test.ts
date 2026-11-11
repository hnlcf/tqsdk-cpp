import { describe, expect, it } from "vitest";
import { drawingLine, isChartDrawing, type ChartDrawing } from "./drawings";
import type { OHLCV } from "../indicators/indicators";

const bars: OHLCV[] = [
  { time: 100 as OHLCV["time"], open: 1, high: 2, low: 1, close: 2, volume: 10 },
  { time: 200 as OHLCV["time"], open: 2, high: 3, low: 1, close: 3, volume: 20 },
  { time: 300 as OHLCV["time"], open: 3, high: 4, low: 2, close: 4, volume: 30 },
];

describe("chart drawings", () => {
  it("validates persisted shapes", () => {
    expect(isChartDrawing({ id: "a", kind: "horizontal", price: 3800 })).toBe(true);
    expect(
      isChartDrawing({ id: "b", kind: "trend", t1: 1, price1: 2, t2: 3, price2: 4 }),
    ).toBe(true);
    expect(isChartDrawing({ id: "c", kind: "horizontal", price: Number.NaN })).toBe(false);
    expect(isChartDrawing({ id: "", kind: "trend" })).toBe(false);
  });

  it("spans horizontal lines across the loaded bars", () => {
    const drawing: ChartDrawing = { id: "h", kind: "horizontal", price: 3900 };
    expect(drawingLine(drawing, bars)).toEqual([
      { time: 100, value: 3900 },
      { time: 300, value: 3900 },
    ]);
  });

  it("normalizes trend endpoints regardless of click order", () => {
    const drawing: ChartDrawing = {
      id: "t",
      kind: "trend",
      t1: 300,
      price1: 4100,
      t2: 100,
      price2: 3800,
    };
    expect(drawingLine(drawing, bars)).toEqual([
      { time: 100, value: 3800 },
      { time: 300, value: 4100 },
    ]);
  });
});
