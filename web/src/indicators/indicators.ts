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

export interface BollResult {
  upper: ValuePoint[];
  middle: ValuePoint[];
  lower: ValuePoint[];
}

export interface MacdResult {
  dif: ValuePoint[];
  dea: ValuePoint[];
  histogram: ValuePoint[];
}

export interface IndicatorConfig {
  maPeriods: number[];
  boll: boolean;
  bollPeriod: number;
  macd: boolean;
  rsi: boolean;
  rsiPeriod: number;
}

export const DEFAULT_INDICATORS: IndicatorConfig = {
  maPeriods: [5, 10, 20],
  boll: false,
  bollPeriod: 20,
  macd: false,
  rsi: false,
  rsiPeriod: 14,
};

function validBars(bars: OHLCV[]): boolean {
  return bars.length > 0 && bars.every((bar) => Number.isFinite(bar.close));
}

/** Simple moving average; output starts at index period-1. */
export function sma(bars: OHLCV[], period: number): ValuePoint[] {
  if (!validBars(bars) || !Number.isInteger(period) || period <= 0) return [];
  const out: ValuePoint[] = [];
  let sum = 0;
  for (let index = 0; index < bars.length; index++) {
    sum += bars[index].close;
    if (index >= period) sum -= bars[index - period].close;
    if (index >= period - 1) {
      out.push({ time: bars[index].time, value: sum / period });
    }
  }
  return out;
}

/** Exponential moving average seeded with the first close. */
export function ema(bars: OHLCV[], period: number): ValuePoint[] {
  if (!validBars(bars) || !Number.isInteger(period) || period <= 0) return [];
  const alpha = 2 / (period + 1);
  let value = bars[0].close;
  const out: ValuePoint[] = [{ time: bars[0].time, value }];
  for (let index = 1; index < bars.length; index++) {
    value = bars[index].close * alpha + value * (1 - alpha);
    out.push({ time: bars[index].time, value });
  }
  return out;
}

/** Bollinger bands using population variance. */
export function boll(
  bars: OHLCV[],
  period = 20,
  multiplier = 2,
): BollResult {
  if (!validBars(bars) || !Number.isInteger(period) || period <= 0) {
    return { upper: [], middle: [], lower: [] };
  }
  const upper: ValuePoint[] = [];
  const middle: ValuePoint[] = [];
  const lower: ValuePoint[] = [];
  let sum = 0;
  let sumSquares = 0;
  for (let index = 0; index < bars.length; index++) {
    const close = bars[index].close;
    sum += close;
    sumSquares += close * close;
    if (index >= period) {
      const old = bars[index - period].close;
      sum -= old;
      sumSquares -= old * old;
    }
    if (index >= period - 1) {
      const mean = sum / period;
      const variance = Math.max(0, sumSquares / period - mean * mean);
      const std = Math.sqrt(variance);
      const time = bars[index].time;
      middle.push({ time, value: mean });
      upper.push({ time, value: mean + multiplier * std });
      lower.push({ time, value: mean - multiplier * std });
    }
  }
  return { upper, middle, lower };
}

/** MACD (DIF = EMA(fast)-EMA(slow), DEA = EMA(DIF, signal), histogram = 2*(DIF-DEA)). */
export function macd(
  bars: OHLCV[],
  fast = 12,
  slow = 26,
  signal = 9,
): MacdResult {
  if (!validBars(bars) || fast <= 0 || slow <= 0 || signal <= 0) {
    return { dif: [], dea: [], histogram: [] };
  }
  const fastEma = ema(bars, fast);
  const slowEma = ema(bars, slow);
  const length = Math.min(fastEma.length, slowEma.length);
  const dif: ValuePoint[] = [];
  for (let index = 0; index < length; index++) {
    dif.push({
      time: fastEma[index].time,
      value: fastEma[index].value - slowEma[index].value,
    });
  }
  const dea: ValuePoint[] = [];
  let deaValue = 0;
  const alpha = 2 / (signal + 1);
  for (let index = 0; index < dif.length; index++) {
    deaValue = index === 0 ? dif[index].value : dif[index].value * alpha + deaValue * (1 - alpha);
    dea.push({ time: dif[index].time, value: deaValue });
  }
  const histogram = dif.map((point, index) => ({
    time: point.time,
    value: 2 * (point.value - dea[index].value),
  }));
  return { dif, dea, histogram };
}

/** Wilder RSI; output starts at index period. */
export function rsi(bars: OHLCV[], period = 14): ValuePoint[] {
  if (!validBars(bars) || period <= 0 || bars.length <= period) return [];
  let gain = 0;
  let loss = 0;
  for (let index = 1; index <= period; index++) {
    const delta = bars[index].close - bars[index - 1].close;
    if (delta >= 0) gain += delta;
    else loss -= delta;
  }
  let avgGain = gain / period;
  let avgLoss = loss / period;
  const out: ValuePoint[] = [];
  const record = (index: number) => {
    const value =
      avgLoss === 0 ? (avgGain === 0 ? 50 : 100) : 100 - 100 / (1 + avgGain / avgLoss);
    out.push({ time: bars[index].time, value });
  };
  record(period);
  for (let index = period + 1; index < bars.length; index++) {
    const delta = bars[index].close - bars[index - 1].close;
    const currentGain = delta > 0 ? delta : 0;
    const currentLoss = delta < 0 ? -delta : 0;
    avgGain = (avgGain * (period - 1) + currentGain) / period;
    avgLoss = (avgLoss * (period - 1) + currentLoss) / period;
    record(index);
  }
  return out;
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
