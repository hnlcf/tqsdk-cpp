import type { OHLCV } from "../indicators";
import type { WasmWorkerResponse } from "./protocol";
import { outputChannelCount, parseManifest, type WasmManifest } from "./types";

interface Pending {
  resolve: (outputs: Float64Array[]) => void;
  reject: (error: Error) => void;
  timer: number;
}

const COMPUTE_TIMEOUT_MS = 5_000;

/**
 * Main-thread handle for one sandboxed WASM indicator.
 *
 * The Wasm module is compiled only inside the dedicated worker. The worker
 * rejects any module that imports host functions; it also validates the raw
 * output size before returning values.
 */
export class WasmIndicatorHost {
  readonly worker: Worker;
  manifest: WasmManifest | undefined;
  private loaded = false;
  private sequence = 0;
  private pending = new Map<number, Pending>();
  private loadResolve: (() => void) | undefined;
  private loadReject: ((error: Error) => void) | undefined;

  constructor(worker?: Worker) {
    this.worker = worker ?? new Worker(new URL("./worker.ts", import.meta.url), { type: "module" });
    this.worker.onmessage = (event: MessageEvent<WasmWorkerResponse>) => this.onMessage(event.data);
    this.worker.onerror = (event) => this.failAll(new Error(event.message || "WASM worker error"));
  }

  async load(manifestUrl: string): Promise<WasmManifest> {
    const response = await fetch(manifestUrl, { credentials: "same-origin" });
    if (!response.ok) {
      throw new Error(`WASM manifest load failed: HTTP ${response.status}`);
    }
    const manifest = parseManifest(await response.json());
    const wasmUrl = new URL(manifest.wasm, new URL(manifestUrl, window.location.href)).toString();
    await new Promise<void>((resolve, reject) => {
      this.loadResolve = resolve;
      this.loadReject = reject;
      this.worker.postMessage({ type: "load", wasmUrl });
    });
    this.manifest = manifest;
    this.loaded = true;
    return manifest;
  }

  compute(bars: OHLCV[], params: number[]): Promise<Float64Array[]> {
    if (!this.loaded || !this.manifest) {
      return Promise.reject(new Error("WASM indicator is not loaded"));
    }
    const id = ++this.sequence;
    return new Promise<Float64Array[]>((resolve, reject) => {
      const timer = window.setTimeout(() => {
        this.pending.delete(id);
        reject(new Error("WASM indicator timed out"));
        // A Wasm infinite loop cannot be interrupted safely; discard the worker.
        this.close();
      }, COMPUTE_TIMEOUT_MS);
      this.pending.set(id, { resolve, reject, timer });
      this.worker.postMessage({
        type: "compute",
        id,
        bars,
        params,
        outputCount: outputChannelCount(this.manifest!),
      });
    });
  }

  close(): void {
    this.worker.terminate();
    this.loaded = false;
    this.failAll(new Error("WASM indicator host closed"));
  }

  private onMessage(message: WasmWorkerResponse): void {
    if (message.type === "loaded") {
      this.loadResolve?.();
      this.loadResolve = undefined;
      this.loadReject = undefined;
      return;
    }
    if (message.type === "error") {
      const error = new Error(message.error);
      if (message.id === undefined) {
        this.loadReject?.(error);
        this.loadResolve = undefined;
        this.loadReject = undefined;
      } else {
        const pending = this.pending.get(message.id);
        if (pending) {
          window.clearTimeout(pending.timer);
          this.pending.delete(message.id);
          pending.reject(error);
        }
      }
      return;
    }
    const pending = this.pending.get(message.id);
    if (!pending) return;
    window.clearTimeout(pending.timer);
    this.pending.delete(message.id);
    pending.resolve(message.outputs);
  }

  private failAll(error: Error): void {
    this.loadReject?.(error);
    this.loadResolve = undefined;
    this.loadReject = undefined;
    for (const pending of this.pending.values()) {
      window.clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.pending.clear();
  }
}
