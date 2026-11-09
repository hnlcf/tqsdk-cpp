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

export interface IndicatorColors {
  ma?: string[];
  bollUpper?: string;
  bollMiddle?: string;
  bollLower?: string;
  macdDif?: string;
  macdDea?: string;
  macdHistUp?: string;
  macdHistDown?: string;
  rsi?: string;
}

export type CustomIndicatorKind = "EMA" | "SMA";

/**
 * Safe declarative custom indicator. Deliberately does not contain executable
 * JavaScript: only whitelisted calculations can run, so a persisted spec cannot
 * access DOM, network, cookies or arbitrary code.
 */
export interface CustomIndicatorSpec {
  id: string;
  kind: CustomIndicatorKind;
  period: number;
  color: string;
  enabled: boolean;
}

export interface IndicatorConfig {
  maPeriods: number[];
  maColors?: string[];
  boll: boolean;
  bollPeriod: number;
  bollMultiplier: number;
  bollColors?: IndicatorColors;
  macd: boolean;
  macdFast: number;
  macdSlow: number;
  macdSignal: number;
  macdColors?: IndicatorColors;
  rsi: boolean;
  rsiPeriod: number;
  rsiColors?: IndicatorColors;
  customIndicators: CustomIndicatorSpec[];
}

export const DEFAULT_INDICATORS: IndicatorConfig = {
  maPeriods: [5, 10, 20],
  boll: false,
  bollPeriod: 20,
  bollMultiplier: 2,
  macd: false,
  macdFast: 12,
  macdSlow: 26,
  macdSignal: 9,
  rsi: false,
  rsiPeriod: 14,
  customIndicators: [
    {
      id: "ema-6",
      kind: "EMA",
      period: 6,
      color: "#f0b90b",
      enabled: true,
    },
  ],
};

function positiveInt(value: unknown, fallback: number): number {
  return typeof value === "number" && Number.isInteger(value) && value > 0 ? value : fallback;
}

const CUSTOM_COLOR = /^#[0-9a-f]{6}$/i;
const MAX_CUSTOM_INDICATORS = 12;

function normalizeCustomIndicators(value: unknown): CustomIndicatorSpec[] {
  if (!Array.isArray(value)) return DEFAULT_INDICATORS.customIndicators;
  const out: CustomIndicatorSpec[] = [];
  const ids = new Set<string>();
  for (const [index, item] of value.entries()) {
    if (!item || typeof item !== "object" || out.length >= MAX_CUSTOM_INDICATORS) continue;
    const row = item as Partial<CustomIndicatorSpec>;
    const kind = row.kind === "SMA" ? "SMA" : row.kind === "EMA" ? "EMA" : null;
    if (!kind) continue;
    const period = positiveInt(row.period, 0);
    if (period < 1 || period > 500) continue;
    let id = typeof row.id === "string" && row.id.trim() ? row.id.trim() : `custom-${index + 1}`;
    while (ids.has(id)) id = `${id}-${index + 1}`;
    ids.add(id);
    out.push({
      id,
      kind,
      period,
      color: typeof row.color === "string" && CUSTOM_COLOR.test(row.color) ? row.color : "#f0b90b",
      enabled: row.enabled !== false,
    });
  }
  return out;
}

/** Run a whitelisted custom calculation. No eval/Function/network is involved. */
export function customIndicator(bars: OHLCV[], spec: CustomIndicatorSpec): ValuePoint[] {
  if (spec.kind === "SMA") return sma(bars, spec.period);
  return ema(bars, spec.period);
}

/** Normalize persisted/legacy configs without losing user customisations. */
export function normalizeIndicatorConfig(config: Partial<IndicatorConfig>): IndicatorConfig {
  const maPeriods = Array.isArray(config.maPeriods)
    ? config.maPeriods.map((item) => positiveInt(item, 0)).filter((item) => item > 0)
    : DEFAULT_INDICATORS.maPeriods;
  return {
    ...DEFAULT_INDICATORS,
    ...config,
    maPeriods: maPeriods.length ? [...new Set(maPeriods)] : [],
    bollPeriod: positiveInt(config.bollPeriod, DEFAULT_INDICATORS.bollPeriod),
    bollMultiplier:
      typeof config.bollMultiplier === "number" && config.bollMultiplier > 0
        ? config.bollMultiplier
        : DEFAULT_INDICATORS.bollMultiplier,
    macdFast: positiveInt(config.macdFast, DEFAULT_INDICATORS.macdFast),
    macdSlow: positiveInt(config.macdSlow, DEFAULT_INDICATORS.macdSlow),
    macdSignal: positiveInt(config.macdSignal, DEFAULT_INDICATORS.macdSignal),
    rsiPeriod: positiveInt(config.rsiPeriod, DEFAULT_INDICATORS.rsiPeriod),
    customIndicators: normalizeCustomIndicators(config.customIndicators),
  };
}

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
