export interface WasmPluginDescriptor {
  id: string;
  manifestUrl: string;
}

/** First-party plugins shipped under web/public/plugins/. */
export const DEFAULT_WASM_PLUGINS: WasmPluginDescriptor[] = [
  {
    id: "ema6",
    manifestUrl: "/plugins/ema6/manifest.json",
  },
];
