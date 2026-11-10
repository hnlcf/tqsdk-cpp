import { describe, expect, it } from "vitest";
import {
  averagePriceLine,
  chinaTimeLabel,
  intradayBars,
  type OHLCV,
} from "./indicators";

function bar(time: number, close: number, volume = 1): OHLCV {
  return { time, open: close, high: close, low: close, close, volume };
}

describe("intraday data", () => {
  it("keeps the current China futures session", () => {
    // 2026-11-08 21:00 CST -> previous night; 2026-11-09 09:00/11:00 CST.
    const night = Date.UTC(2026, 10, 8, 13, 0) / 1000;
    const morning = Date.UTC(2026, 10, 9, 1, 0) / 1000;
    const late = Date.UTC(2026, 10, 9, 3, 0) / 1000;
    const selected = intradayBars([bar(night, 1), bar(morning, 2), bar(late, 3)]);
    expect(selected.map((item) => item.time)).toEqual([night, morning, late]);
    expect(chinaTimeLabel(night)).toBe("21:00");
  });

  it("computes cumulative volume-weighted average price", () => {
    const points = averagePriceLine([
      bar(1, 10, 2),
      bar(2, 20, 1),
      { time: 3, open: 15, high: 15, low: 15, close: 15, volume: 0 },
    ]);
    expect(points.map((point) => point.value)).toEqual([10, 40 / 3, 40 / 3]);
  });

  it("returns a bounded tail when filtering removes all bars", () => {
    const bars = [bar(100, 1), bar(200, 2), bar(300, 3)];
    expect(intradayBars(bars, 2).map((item) => item.time)).toEqual([200, 300]);
  });
});
