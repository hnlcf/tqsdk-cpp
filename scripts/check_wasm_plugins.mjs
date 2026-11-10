#!/usr/bin/env node
// Smoke-check every checked-in WASM indicator against the TQW1 ABI.
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";

const ROOT = new URL("..", import.meta.url).pathname;
const PLUGIN_ROOT = join(ROOT, "web/public/plugins");
const HEADER = 24;
const MAGIC = 0x31575154;
const FIELD_COUNT = 5;

function bars(count = 120) {
  return Array.from({ length: count }, (_, index) => {
    const close = 100 + Math.sin(index / 7) * 8 + index * 0.02;
    return {
      open: close - 0.4,
      high: close + 0.8,
      low: close - 0.8,
      close,
      volume: 1000 + index,
    };
  });
}

function encode(rows, params, outputCount) {
  const buffer = new ArrayBuffer(
    HEADER + params.length * 8 + FIELD_COUNT * rows.length * 8,
  );
  const view = new DataView(buffer);
  view.setUint32(0, MAGIC, true);
  view.setUint32(4, 1, true);
  view.setUint32(8, rows.length, true);
  view.setUint32(12, params.length, true);
  view.setUint32(16, outputCount, true);
  let offset = HEADER;
  for (const value of params) {
    view.setFloat64(offset, value, true);
    offset += 8;
  }
  for (const field of ["open", "high", "low", "close", "volume"]) {
    for (const row of rows) {
      view.setFloat64(offset, row[field], true);
      offset += 8;
    }
  }
  return buffer;
}

let failed = 0;
for (const entry of readdirSync(PLUGIN_ROOT, { withFileTypes: true })) {
  if (!entry.isDirectory()) continue;
  const directory = join(PLUGIN_ROOT, entry.name);
  const manifest = JSON.parse(readFileSync(join(directory, "manifest.json"), "utf8"));
  const module = await WebAssembly.compile(
    readFileSync(join(directory, manifest.wasm)),
  );
  const imports = WebAssembly.Module.imports(module);
  if (imports.length !== 0) {
    console.error(`${manifest.id}: unexpected imports`, imports);
    failed++;
    continue;
  }
  const instance = await WebAssembly.instantiate(module, {});
  const { exports } = instance;
  const rows = bars();
  const params = manifest.params.map((param) => param.default);
  const outputCount = manifest.outputs.reduce(
    (sum, output) => sum + (output.channels ?? 1),
    0,
  );
  const input = encode(rows, params, outputCount);
  const inputPtr = exports.alloc(input.byteLength);
  new Uint8Array(exports.memory.buffer, inputPtr, input.byteLength).set(
    new Uint8Array(input),
  );
  const outputBytes = outputCount * rows.length * 8;
  const outputPtr = exports.alloc(outputBytes);
  const written = exports.run(inputPtr, input.byteLength, outputPtr, outputBytes);
  const output = new Float64Array(
    exports.memory.buffer.slice(outputPtr, outputPtr + outputBytes),
  );
  const finite = output.reduce((sum, value) => sum + (Number.isFinite(value) ? 1 : 0), 0);
  const ok = written === outputBytes && finite > 0;
  console.log(
    `${ok ? "PASS" : "FAIL"} ${manifest.id.padEnd(10)} outputs=${outputCount} ` +
      `bytes=${written}/${outputBytes} finite=${finite}`,
  );
  if (!ok) failed++;
}

if (failed > 0) {
  console.error(`${failed} plugin(s) failed`);
  process.exit(1);
}
