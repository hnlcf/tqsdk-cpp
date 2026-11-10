export interface WasmPluginDescriptor {
  id: string;
  manifestUrl: string;
  defaultEnabled: boolean;
}

/**
 * First-party plugins shipped under web/public/plugins/.
 *
 * Order controls the settings panel order and legend order. BOLL/MACD/RSI
 * remain opt-in, matching the previous built-in defaults; MA/EMA plus the new
 * ATR/Donchian indicators are visible by default.
 */
export const DEFAULT_WASM_PLUGINS: WasmPluginDescriptor[] = [
  { id: "ema6", manifestUrl: "/plugins/ema6/manifest.json", defaultEnabled: true },
  { id: "ma", manifestUrl: "/plugins/ma/manifest.json", defaultEnabled: true },
  { id: "boll", manifestUrl: "/plugins/boll/manifest.json", defaultEnabled: false },
  { id: "macd", manifestUrl: "/plugins/macd/manifest.json", defaultEnabled: false },
  { id: "rsi", manifestUrl: "/plugins/rsi/manifest.json", defaultEnabled: false },
  { id: "atr", manifestUrl: "/plugins/atr/manifest.json", defaultEnabled: true },
  { id: "donchian", manifestUrl: "/plugins/donchian/manifest.json", defaultEnabled: true },
];
