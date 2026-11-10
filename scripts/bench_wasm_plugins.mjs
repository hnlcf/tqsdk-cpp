#!/usr/bin/env node
// Lightweight ABI benchmark for checked-in WASM indicators.
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { performance } from "node:perf_hooks";

const ROOT = new URL("..", import.meta.url).pathname;
const PLUGIN_ROOT = join(ROOT, "web/public/plugins");
const HEADER = 24;
const FIELD_COUNT = 5;
const BARS = 5000;
const ITERATIONS = 100;

function rows() {
  return Array.from({ length: BARS }, (_, index) => {
    const close = 3000 + Math.sin(index / 19) * 12 + Math.cos(index / 7) * 4;
    return {
      open: close - 0.3,
      high: close + 0.6,
      low: close - 0.6,
      close,
      volume: 100 + (index % 37),
    };
  });
}

function input(bars, params, outputCount) {
  const buffer = new ArrayBuffer(HEADER + params.length * 8 + FIELD_COUNT * bars.length * 8);
  const view = new DataView(buffer);
  view.setUint32(0, 0x31575154, true);
  view.setUint32(4, 1, true);
  view.setUint32(8, bars.length, true);
  view.setUint32(12, params.length, true);
  view.setUint32(16, outputCount, true);
  let offset = HEADER;
  for (const value of params) {
    view.setFloat64(offset, value, true);
    offset += 8;
  }
  for (const field of ["open", "high", "low", "close", "volume"]) {
    for (const bar of bars) {
      view.setFloat64(offset, bar[field], true);
      offset += 8;
    }
  }
  return buffer;
}

const bars = rows();
const results = [];
for (const entry of readdirSync(PLUGIN_ROOT, { withFileTypes: true })) {
  if (!entry.isDirectory()) continue;
  const directory = join(PLUGIN_ROOT, entry.name);
  const manifest = JSON.parse(readFileSync(join(directory, "manifest.json"), "utf8"));
  const module = await WebAssembly.compile(readFileSync(join(directory, manifest.wasm)));
  const { exports } = await WebAssembly.instantiate(module, {});
  const params = manifest.params.map((param) => param.default);
  const outputCount = manifest.outputs.reduce(
    (sum, output) => sum + (output.channels ?? 1),
    0,
  );
  const encoded = input(bars, params, outputCount);
  const outputBytes = outputCount * bars.length * 8;
  const inputPtr = exports.alloc(encoded.byteLength);
  new Uint8Array(exports.memory.buffer, inputPtr, encoded.byteLength).set(
    new Uint8Array(encoded),
  );
  const outputPtr = exports.alloc(outputBytes);
  // Warm-up: module loading/allocation is excluded from the measurement.
  exports.run(inputPtr, encoded.byteLength, outputPtr, outputBytes);
  const started = performance.now();
  for (let index = 0; index < ITERATIONS; index++) {
    const written = exports.run(
      inputPtr,
      encoded.byteLength,
      outputPtr,
      outputBytes,
    );
    if (written !== outputBytes) throw new Error(`${manifest.id}: ABI failure`);
  }
  const elapsed = performance.now() - started;
  results.push({
    id: manifest.id,
    bars: BARS,
    iterations: ITERATIONS,
    ms_per_run: elapsed / ITERATIONS,
    runs_per_second: (ITERATIONS * 1000) / elapsed,
  });
}

results.sort((a, b) => a.id.localeCompare(b.id));
console.log(JSON.stringify({ bars: BARS, iterations: ITERATIONS, results }, null, 2));
