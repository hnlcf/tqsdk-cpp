import { describe, expect, it } from "vitest";
import {
  boll,
  chinaTimeLabel,
  ema,
  intradayBars,
  normalizeIndicatorConfig,
  macd,
  rsi,
  sma,
  type OHLCV,
} from "./indicators";

function bar(time: number, close: number, volume = 1): OHLCV {
  return { time, open: close, high: close, low: close, close, volume };
}

describe("indicators", () => {
  const bars = [1, 2, 3, 4, 5].map((value, index) => bar(index, value));

  it("computes simple moving averages", () => {
    expect(sma(bars, 3).map((point) => point.value)).toEqual([2, 3, 4]);
  });

  it("computes exponential moving averages", () => {
    const values = ema(bars, 3);
    expect(values).toHaveLength(5);
    expect(values[0].value).toBe(1);
    expect(values.at(-1)!.value).toBeGreaterThan(4);
    expect(values.at(-1)!.value).toBeLessThan(5);
  });

  it("computes bollinger bands around the SMA", () => {
    const bands = boll(bars, 3, 2);
    expect(bands.middle.map((point) => point.value)).toEqual([2, 3, 4]);
    expect(bands.upper[0].value).toBeGreaterThan(bands.middle[0].value);
    expect(bands.lower[0].value).toBeLessThan(bands.middle[0].value);
  });

  it("computes MACD line lengths and histogram", () => {
    const result = macd(bars, 2, 4, 2);
    expect(result.dif.length).toBeGreaterThan(0);
    expect(result.dif.length).toBe(result.dea.length);
    expect(result.histogram.length).toBe(result.dif.length);
  });

  it("returns RSI 100 for a monotonically rising series", () => {
    const result = rsi(bars, 2);
    expect(result.length).toBe(3);
    expect(result[0].value).toBe(100);
  });

  it("normalizes legacy and invalid indicator parameters", () => {
    const config = normalizeIndicatorConfig({
      maPeriods: [5, 5, 0],
      bollPeriod: 0,
      bollMultiplier: -1,
      macdFast: 0,
      rsiPeriod: 0,
    });
    expect(config.maPeriods).toEqual([5]);
    expect(config.bollPeriod).toBe(20);
    expect(config.bollMultiplier).toBe(2);
    expect(config.rsiPeriod).toBe(14);
  });

  it("keeps the current China futures session for intraday charts", () => {
    // 2026-11-08 21:00 CST -> previous night; 2026-11-09 09:00/11:00 CST.
    const night = Date.UTC(2026, 10, 8, 13, 0) / 1000;
    const morning = Date.UTC(2026, 10, 9, 1, 0) / 1000;
    const late = Date.UTC(2026, 10, 9, 3, 0) / 1000;
    const selected = intradayBars([bar(night, 1), bar(morning, 2), bar(late, 3)]);
    expect(selected.map((item) => item.time)).toEqual([night, morning, late]);
    expect(chinaTimeLabel(night)).toBe("21:00");
  });
});
