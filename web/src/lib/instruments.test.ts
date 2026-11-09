import { describe, expect, it } from "vitest";
import {
  fallbackVolumeMultiple,
  instrumentName,
  productOf,
  searchFallbackInstruments,
} from "./instruments";

describe("instrument metadata", () => {
  it("derives product and display name from a contract symbol", () => {
    expect(productOf("CZCE.SA701")).toBe("SA");
    expect(instrumentName("CZCE.SA701")).toContain("纯碱");
    expect(fallbackVolumeMultiple("CZCE.SA701")).toBe(20);
  });

  it("supports fuzzy code and Chinese-name search without catalog", () => {
    expect(searchFallbackInstruments("rb").map((item) => item.symbol)).toContain("SHFE.rb2701");
    expect(searchFallbackInstruments("螺纹").map((item) => item.symbol)).toContain("SHFE.rb2701");
    expect(searchFallbackInstruments("SA").map((item) => item.symbol)).toContain("CZCE.SA701");
  });
});
