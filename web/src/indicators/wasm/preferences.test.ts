import { describe, expect, it } from "vitest";
import { applyWasmPreference } from "./preferences";
import { parseManifest } from "./types";

const manifest = parseManifest({
  schema: 1,
  abi: 1,
  id: "ema6",
  name: "EMA 多均线",
  version: "0.2.0",
  wasm: "ema6.wasm",
  dynamicLines: true,
  defaultEnabled: true,
  order: 10,
  params: [
    { id: "period1", label: "EMA1", type: "int", default: 5, min: 1, max: 500 },
    { id: "period2", label: "EMA2", type: "int", default: 10, min: 1, max: 500 },
  ],
  outputs: [
    {
      id: "ema1",
      title: "EMA{period1}",
      pane: "main",
      plot: "line",
      color: "#f0b90b",
      periodParam: "period1",
    },
    {
      id: "ema2",
      title: "EMA{period2}",
      pane: "main",
      plot: "line",
      color: "#53b7ff",
      defaultVisible: false,
      periodParam: "period2",
    },
  ],
});

describe("WASM indicator preferences", () => {
  it("restores and clamps persisted values/outputs", () => {
    const applied = applyWasmPreference(manifest, true, {
      enabled: false,
      values: [999, 7.6],
      outputConfig: {
        ema1: {
          visible: false,
          color: "#00ff00",
          lineStyle: "dotted",
          lineWidth: 3,
        },
        ema2: { visible: true },
      },
    });
    expect(applied.enabled).toBe(false);
    expect(applied.values).toEqual([500, 8]);
    expect(applied.outputConfig.ema1).toEqual({
      visible: false,
      color: "#00ff00",
      lineStyle: "dotted",
      lineWidth: 3,
    });
    expect(applied.outputConfig.ema2.visible).toBe(true);
    expect(applied.outputConfig.ema2.color).toBe("#53b7ff");
  });

  it("falls back to manifest defaults for invalid preferences", () => {
    const applied = applyWasmPreference(manifest, true, {
      values: [Number.NaN],
      outputConfig: {
        ema1: { color: "red", lineStyle: "weird" as never, lineWidth: 99 },
      },
    });
    expect(applied.values).toEqual([5, 10]);
    expect(applied.outputConfig.ema1.color).toBe("#f0b90b");
    expect(applied.outputConfig.ema1.lineStyle).toBe("solid");
    expect(applied.outputConfig.ema1.lineWidth).toBe(4);
  });
});
