import type { LineData, UTCTimestamp } from "lightweight-charts";
import type { OHLCV } from "../indicators/indicators";

export type DrawMode = "none" | "horizontal" | "trend";

export interface HorizontalDrawing {
  id: string;
  kind: "horizontal";
  price: number;
}

export interface TrendDrawing {
  id: string;
  kind: "trend";
  t1: number;
  price1: number;
  t2: number;
  price2: number;
}

export type ChartDrawing = HorizontalDrawing | TrendDrawing;
export type NewChartDrawing =
  | Omit<HorizontalDrawing, "id">
  | Omit<TrendDrawing, "id">;

export function isChartDrawing(value: unknown): value is ChartDrawing {
  if (!value || typeof value !== "object") return false;
  const row = value as Record<string, unknown>;
  if (typeof row.id !== "string" || row.id.length === 0) return false;
  if (row.kind === "horizontal") {
    return typeof row.price === "number" && Number.isFinite(row.price);
  }
  if (row.kind === "trend") {
    return (
      typeof row.t1 === "number" &&
      Number.isFinite(row.t1) &&
      typeof row.price1 === "number" &&
      Number.isFinite(row.price1) &&
      typeof row.t2 === "number" &&
      Number.isFinite(row.t2) &&
      typeof row.price2 === "number" &&
      Number.isFinite(row.price2)
    );
  }
  return false;
}

/** Build two-point line data; horizontal lines span the first/last visible bar. */
export function drawingLine(
  drawing: ChartDrawing,
  bars: OHLCV[],
): LineData<UTCTimestamp>[] {
  if (drawing.kind === "horizontal") {
    if (bars.length === 0) return [];
    return [
      { time: bars[0].time as UTCTimestamp, value: drawing.price },
      { time: bars[bars.length - 1].time as UTCTimestamp, value: drawing.price },
    ];
  }
  const start = Math.min(drawing.t1, drawing.t2);
  const end = Math.max(drawing.t1, drawing.t2);
  const startPrice = drawing.t1 <= drawing.t2 ? drawing.price1 : drawing.price2;
  const endPrice = drawing.t1 <= drawing.t2 ? drawing.price2 : drawing.price1;
  return [
    { time: start as UTCTimestamp, value: startPrice },
    { time: end as UTCTimestamp, value: endPrice },
  ];
}
