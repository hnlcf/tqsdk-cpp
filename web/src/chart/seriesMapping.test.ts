import { describe, expect, it } from "vitest";
import { LineStyle } from "lightweight-charts";
import type { OHLCV } from "../indicators/indicators";
import { toCandlestick, toLineData, toLineStyle, toVolumeData } from "./seriesMapping";

const bars: OHLCV[] = [
  { time: 100, open: 10, high: 12, low: 9, close: 11, volume: 20 },
  { time: 200, open: 11, high: 13, low: 10, close: 10, volume: 30 },
];

describe("seriesMapping", () => {
  it("maps candles and line styles", () => {
    expect(toCandlestick(bars[0])).toEqual({
      time: 100,
      open: 10,
      high: 12,
      low: 9,
      close: 11,
    });
    expect(toLineStyle("dashed")).toBe(LineStyle.Dashed);
    expect(toLineStyle("dotted")).toBe(LineStyle.Dotted);
    expect(toLineStyle(undefined)).toBe(LineStyle.Solid);
  });

  it("filters non-finite values and truncates to the shorter array", () => {
    expect(toLineData(bars, new Float64Array([1, Number.NaN]))).toEqual([{ time: 100, value: 1 }]);
    expect(toLineData(bars, new Float64Array([1]))).toEqual([{ time: 100, value: 1 }]);
  });

  it("colors volume by candle direction", () => {
    expect(toVolumeData(bars, "#0f0", "#f00")).toEqual([
      { time: 100, value: 20, color: "#0f0" },
      { time: 200, value: 30, color: "#f00" },
    ]);
  });
});
