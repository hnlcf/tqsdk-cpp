/// <reference lib="webworker" />
import { decodeWasmOutput, encodeWasmInput } from "./input";
import type { WasmWorkerRequest, WasmWorkerResponse } from "./protocol";

interface WasmExports {
  memory: WebAssembly.Memory;
  alloc: (size: number) => number;
  dealloc: (ptr: number, size: number) => void;
  run: (inputPtr: number, inputLen: number, outputPtr: number, outputCap: number) => number;
}

let wasm: WasmExports | undefined;

function post(message: WasmWorkerResponse, transfer: Transferable[] = []) {
  self.postMessage(message, transfer);
}

function errorText(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

async function load(wasmUrl: string) {
  const response = await fetch(wasmUrl, { credentials: "same-origin" });
  if (!response.ok) {
    throw new Error(`WASM plugin load failed: HTTP ${response.status}`);
  }
  const module = await WebAssembly.compile(await response.arrayBuffer());
  const imports = WebAssembly.Module.imports(module);
  if (imports.length > 0) {
    throw new Error(
      `WASM plugin must not import host functions (got ${imports.map((item) => item.module + "." + item.name).join(", ")})`,
    );
  }
  const instance = await WebAssembly.instantiate(module, {});
  const exports = instance.exports as unknown as Partial<WasmExports>;
  if (
    !(exports.memory instanceof WebAssembly.Memory) ||
    typeof exports.alloc !== "function" ||
    typeof exports.dealloc !== "function" ||
    typeof exports.run !== "function"
  ) {
    throw new Error("WASM plugin must export memory, alloc, dealloc and run");
  }
  wasm = exports as WasmExports;
}

function compute(request: Extract<WasmWorkerRequest, { type: "compute" }>) {
  if (!wasm) throw new Error("WASM plugin is not loaded");
  const input = encodeWasmInput(request.bars, request.params, request.outputCount);
  const outputBytes = request.outputCount * request.bars.length * 8;
  const inputPtr = wasm.alloc(input.byteLength) >>> 0;
  const outputPtr = wasm.alloc(outputBytes) >>> 0;
  if (inputPtr === 0 || (outputBytes > 0 && outputPtr === 0)) {
    if (inputPtr !== 0) wasm.dealloc(inputPtr, input.byteLength);
    throw new Error("WASM plugin allocation failed");
  }
  try {
    new Uint8Array(wasm.memory.buffer, inputPtr, input.byteLength).set(new Uint8Array(input));
    const written = wasm.run(inputPtr, input.byteLength, outputPtr, outputBytes);
    if (written !== outputBytes) {
      throw new Error(`WASM plugin returned ${written} bytes, expected ${outputBytes}`);
    }
    const bytes = new Uint8Array(wasm.memory.buffer, outputPtr, outputBytes).slice();
    const outputs = decodeWasmOutput(bytes.buffer, request.outputCount, request.bars.length);
    return outputs;
  } finally {
    wasm.dealloc(inputPtr, input.byteLength);
    if (outputBytes > 0) wasm.dealloc(outputPtr, outputBytes);
  }
}

self.onmessage = async (event: MessageEvent<WasmWorkerRequest>) => {
  const request = event.data;
  if (request.type === "load") {
    try {
      await load(request.wasmUrl);
      post({ type: "loaded" });
    } catch (error) {
      wasm = undefined;
      post({ type: "error", error: errorText(error) });
    }
    return;
  }

  try {
    const outputs = compute(request);
    post(
      { type: "result", id: request.id, outputs },
      outputs.map((values) => values.buffer),
    );
  } catch (error) {
    post({ type: "error", id: request.id, error: errorText(error) });
  }
};
