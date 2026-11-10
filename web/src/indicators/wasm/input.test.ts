import { describe, expect, it } from "vitest";
import type { OHLCV } from "../indicators";
import {
  decodeWasmOutput,
  encodeWasmInput,
  WASM_HEADER_BYTES,
  WASM_INPUT_MAGIC,
} from "./input";
import { outputChannelCount, parseManifest } from "./types";

function bar(time: number, close: number): OHLCV {
  return { time, open: close, high: close + 1, low: close - 1, close, volume: 10 };
}

describe("WASM indicator ABI", () => {
  it("encodes aligned OHLCV/parameter buffers and decodes outputs", () => {
    const bars = [bar(1, 10), bar(2, 20)];
    const input = encodeWasmInput(bars, [6], 1);
    const view = new DataView(input);
    expect(view.getUint32(0, true)).toBe(WASM_INPUT_MAGIC);
    expect(view.getUint32(4, true)).toBe(1);
    expect(view.getUint32(8, true)).toBe(2);
    expect(view.getUint32(12, true)).toBe(1);
    expect(view.getUint32(16, true)).toBe(1);
    expect(view.getUint32(20, true)).toBe(0);
    expect(view.getFloat64(WASM_HEADER_BYTES, true)).toBe(6);
    const closeOffset = WASM_HEADER_BYTES + 8 + 3 * 2 * 8;
    expect(view.getFloat64(closeOffset, true)).toBe(10);
    expect(view.getFloat64(closeOffset + 8, true)).toBe(20);

    const out = new Float64Array([1, 2]);
    const decoded = decodeWasmOutput(out.buffer, 1, 2);
    expect([...decoded[0]]).toEqual([1, 2]);
  });

  it("validates manifest shape and rejects executable path tricks", () => {
    const manifest = parseManifest({
      schema: 1,
      abi: 1,
      id: "ema6",
      name: "EMA6",
      version: "0.1.0",
      wasm: "ema6.wasm",
      params: [{ id: "period", label: "周期", type: "int", default: 6, min: 1, max: 500 }],
      outputs: [{ id: "ema", title: "EMA6", pane: "main", plot: "line", color: "#f0b90b" }],
    });
    expect(manifest.outputs[0].lineWidth).toBe(1);
    const band = parseManifest({
      ...manifest,
      outputs: [{ ...manifest.outputs[0], plot: "band", channels: undefined }],
    });
    expect(outputChannelCount(band)).toBe(2);
    expect(() =>
      parseManifest({
        ...manifest,
        wasm: "../outside.wasm",
      }),
    ).toThrow(/relative/);
    expect(() =>
      parseManifest({
        ...manifest,
        outputs: [{ ...manifest.outputs[0], plot: "custom-draw" }],
      }),
    ).toThrow(/unsupported/);
  });
});
