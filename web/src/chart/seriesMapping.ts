import {
  LineStyle,
  type CandlestickData,
  type HistogramData,
  type LineData,
  type UTCTimestamp,
} from "lightweight-charts";
import type { OHLCV } from "../indicators/indicators";
import type { WasmLineStyle } from "../indicators/wasm/types";

/** Pure mappings used by the chart; kept browser-independent for tests. */
export function toCandlestick(bar: OHLCV): CandlestickData<UTCTimestamp> {
  return {
    time: bar.time as UTCTimestamp,
    open: bar.open,
    high: bar.high,
    low: bar.low,
    close: bar.close,
  };
}

export function toLineStyle(style: WasmLineStyle | undefined): LineStyle {
  if (style === "dashed") return LineStyle.Dashed;
  if (style === "dotted") return LineStyle.Dotted;
  return LineStyle.Solid;
}

export function formatLegendPrice(value: number | undefined, decimals: number): string {
  return typeof value === "number" && Number.isFinite(value) ? value.toFixed(decimals) : "—";
}

/** Convert finite indicator outputs to Lightweight Charts line data. */
export function toLineData(bars: OHLCV[], values: Float64Array): LineData<UTCTimestamp>[] {
  const length = Math.min(bars.length, values.length);
  const data: LineData<UTCTimestamp>[] = [];
  for (let index = 0; index < length; index++) {
    const value = values[index];
    if (Number.isFinite(value)) {
      data.push({ time: bars[index].time as UTCTimestamp, value });
    }
  }
  return data;
}

/** Volume histogram data with up/down colors chosen by candle direction. */
export function toVolumeData(
  bars: OHLCV[],
  upColor: string,
  downColor: string,
): HistogramData<UTCTimestamp>[] {
  return bars.map((bar) => ({
    time: bar.time as UTCTimestamp,
    value: bar.volume,
    color: bar.close >= bar.open ? upColor : downColor,
  }));
}
