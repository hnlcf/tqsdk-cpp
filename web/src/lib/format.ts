import type { Quote } from "../api/client";

export function finite(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value);
}

export function priceDecimals(quote?: Quote, fallback = 2): number {
  if (quote && finite(quote.price_decs)) return Math.max(0, Math.min(6, quote.price_decs));
  return fallback;
}

export function formatPrice(value: unknown, decimals = 2): string {
  return finite(value) ? value.toLocaleString("zh-CN", {
    minimumFractionDigits: decimals,
    maximumFractionDigits: decimals,
  }) : "—";
}

export function formatMoney(value: unknown): string {
  return finite(value)
    ? value.toLocaleString("zh-CN", { minimumFractionDigits: 2, maximumFractionDigits: 2 })
    : "—";
}

export function formatVolume(value: unknown): string {
  return finite(value) ? value.toLocaleString("zh-CN") : "—";
}

export function quotePrevious(quote?: Quote): number | undefined {
  if (!quote) return undefined;
  if (finite(quote.pre_settlement) && quote.pre_settlement > 0) return quote.pre_settlement;
  if (finite(quote.pre_close) && quote.pre_close > 0) return quote.pre_close;
  return undefined;
}

export function quoteChange(quote?: Quote): { change: number; pct: number } {
  if (!quote || !finite(quote.last_price)) return { change: 0, pct: 0 };
  const previous = quotePrevious(quote);
  if (!previous) return { change: 0, pct: 0 };
  const change = quote.last_price - previous;
  return { change, pct: (change / previous) * 100 };
}

export function formatSigned(value: number, decimals = 2): string {
  return `${value >= 0 ? "+" : ""}${value.toFixed(decimals)}`;
}
