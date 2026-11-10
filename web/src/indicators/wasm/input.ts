import type { OHLCV } from "../indicators";

/**
 * TQW1 input layout (little endian):
 *
 *   u32 magic       0x31575154 ("TQW1")
 *   u32 version     1
 *   u32 bar_count
 *   u32 param_count
 *   u32 output_count
 *   u32 reserved
 *   f64 params[param_count]
 *   f64 open[bar_count]
 *   f64 high[bar_count]
 *   f64 low[bar_count]
 *   f64 close[bar_count]
 *   f64 volume[bar_count]
 *
 * The 24-byte header keeps every f64 array 8-byte aligned. The fixed field
 * order is part of ABI 1; adding fields requires an ABI version bump.
 */
export const WASM_INPUT_MAGIC = 0x31575154;
export const WASM_ABI_VERSION = 1;
export const WASM_HEADER_BYTES = 24;
export const WASM_FIELD_COUNT = 5;
export const WASM_MAX_BARS = 20_000;
export const WASM_MAX_PARAMS = 8;
export const WASM_MAX_OUTPUTS = 16;

export function encodeWasmInput(
  bars: OHLCV[],
  params: number[],
  outputCount: number,
): ArrayBuffer {
  if (bars.length > WASM_MAX_BARS) {
    throw new Error(`too many bars for WASM indicator: ${bars.length} > ${WASM_MAX_BARS}`);
  }
  if (params.length > WASM_MAX_PARAMS) {
    throw new Error(`too many parameters for WASM indicator: ${params.length}`);
  }
  if (outputCount < 1 || outputCount > WASM_MAX_OUTPUTS) {
    throw new Error(`invalid WASM output count: ${outputCount}`);
  }
  if (params.some((value) => !Number.isFinite(value))) {
    throw new Error("WASM indicator parameters must be finite numbers");
  }

  const byteLength =
    WASM_HEADER_BYTES + params.length * Float64Array.BYTES_PER_ELEMENT +
    WASM_FIELD_COUNT * bars.length * Float64Array.BYTES_PER_ELEMENT;
  const buffer = new ArrayBuffer(byteLength);
  const view = new DataView(buffer);
  view.setUint32(0, WASM_INPUT_MAGIC, true);
  view.setUint32(4, WASM_ABI_VERSION, true);
  view.setUint32(8, bars.length, true);
  view.setUint32(12, params.length, true);
  view.setUint32(16, outputCount, true);
  view.setUint32(20, 0, true);

  let offset = WASM_HEADER_BYTES;
  for (const param of params) {
    view.setFloat64(offset, param, true);
    offset += 8;
  }

  const fields: Array<(bar: OHLCV) => number> = [
    (bar) => bar.open,
    (bar) => bar.high,
    (bar) => bar.low,
    (bar) => bar.close,
    (bar) => bar.volume,
  ];
  for (const read of fields) {
    for (const bar of bars) {
      view.setFloat64(offset, read(bar), true);
      offset += 8;
    }
  }
  return buffer;
}

export function decodeWasmOutput(
  buffer: ArrayBuffer,
  outputCount: number,
  barCount: number,
): Float64Array[] {
  const valuesPerOutput = barCount;
  const expected = outputCount * valuesPerOutput * Float64Array.BYTES_PER_ELEMENT;
  if (buffer.byteLength !== expected) {
    throw new Error(
      `WASM output size mismatch: got ${buffer.byteLength}, expected ${expected}`,
    );
  }
  const all = new Float64Array(buffer);
  const outputs: Float64Array[] = [];
  for (let index = 0; index < outputCount; index++) {
    outputs.push(all.slice(index * valuesPerOutput, (index + 1) * valuesPerOutput));
  }
  return outputs;
}
