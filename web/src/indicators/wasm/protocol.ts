import type { OHLCV } from "../indicators";

export type WasmWorkerRequest =
  | { type: "load"; wasmUrl: string }
  | { type: "compute"; id: number; bars: OHLCV[]; params: number[]; outputCount: number };

export type WasmWorkerResponse =
  | { type: "loaded" }
  | { type: "result"; id: number; outputs: Float64Array[] }
  | { type: "error"; id?: number; error: string };
