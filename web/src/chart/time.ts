import { TickMarkType, type Time } from "lightweight-charts";

/** Futures charts use China wall-clock time regardless of browser timezone. */
export const CHINA_OFFSET_SECONDS = 8 * 3600;

export function chinaDate(seconds: number): Date {
  return new Date((seconds + CHINA_OFFSET_SECONDS) * 1000);
}

export function two(value: number): string {
  return String(value).padStart(2, "0");
}

export function chinaDateTime(seconds: number): string {
  const date = chinaDate(seconds);
  return `${date.getUTCFullYear()}-${two(date.getUTCMonth() + 1)}-${two(date.getUTCDate())} ${two(
    date.getUTCHours(),
  )}:${two(date.getUTCMinutes())}`;
}

export function chinaTickMark(time: Time, type: TickMarkType): string {
  if (typeof time !== "number") return "";
  const date = chinaDate(time);
  switch (type) {
    case TickMarkType.Year:
      return String(date.getUTCFullYear());
    case TickMarkType.Month:
      return `${date.getUTCFullYear()}-${two(date.getUTCMonth() + 1)}`;
    case TickMarkType.DayOfMonth:
      return `${two(date.getUTCMonth() + 1)}-${two(date.getUTCDate())}`;
    case TickMarkType.Time:
    default:
      return `${two(date.getUTCHours())}:${two(date.getUTCMinutes())}`;
  }
}
