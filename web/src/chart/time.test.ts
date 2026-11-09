import { describe, expect, it } from "vitest";
import { TickMarkType, type UTCTimestamp } from "lightweight-charts";
import { chinaDateTime, chinaTickMark } from "./time";

describe("china chart time", () => {
  const timestamp = (Date.UTC(2026, 10, 9, 13, 0, 0) / 1000) as UTCTimestamp;

  it("formats UTC timestamps as China wall-clock time", () => {
    expect(chinaDateTime(timestamp)).toBe("2026-11-09 21:00");
    expect(chinaTickMark(timestamp, TickMarkType.Time)).toBe("21:00");
    expect(chinaTickMark(timestamp, TickMarkType.DayOfMonth)).toBe("11-09");
  });
});
