import {
  defaultOutputConfig,
  type WasmManifest,
  type WasmOutputConfig,
  type WasmPluginView,
} from "./types";

const KEY_WASM_INDICATORS = "tqsdk.wasm.indicators.v1";
const STYLE_VALUES = new Set(["solid", "dashed", "dotted"]);

interface StoredPluginPreference {
  enabled?: boolean;
  values?: number[];
  outputConfig?: Record<string, Partial<WasmOutputConfig>>;
}

export interface WasmPreferences {
  version: 1;
  plugins: Record<string, StoredPluginPreference>;
}

export interface AppliedWasmPreference {
  enabled: boolean;
  values: number[];
  outputConfig: Record<string, WasmOutputConfig>;
}

function readRaw(): unknown {
  try {
    const raw = window.localStorage.getItem(KEY_WASM_INDICATORS);
    return raw ? JSON.parse(raw) : undefined;
  } catch {
    return undefined;
  }
}

export function readWasmPreferences(): WasmPreferences {
  const raw = readRaw();
  if (!raw || typeof raw !== "object") return { version: 1, plugins: {} };
  const row = raw as Record<string, unknown>;
  if (row.version !== 1 || !row.plugins || typeof row.plugins !== "object") {
    return { version: 1, plugins: {} };
  }
  return { version: 1, plugins: row.plugins as Record<string, StoredPluginPreference> };
}

export function applyWasmPreference(
  manifest: WasmManifest,
  defaultEnabled: boolean,
  stored?: StoredPluginPreference,
): AppliedWasmPreference {
  const values = manifest.params.map((param, index) => {
    const raw = stored?.values?.[index];
    if (typeof raw !== "number" || !Number.isFinite(raw)) return param.default;
    const clamped = Math.min(
      param.max ?? Number.POSITIVE_INFINITY,
      Math.max(param.min ?? Number.NEGATIVE_INFINITY, raw),
    );
    return param.type === "int" ? Math.round(clamped) : clamped;
  });
  const outputConfig: Record<string, WasmOutputConfig> = {};
  for (const output of manifest.outputs) {
    const base = defaultOutputConfig(output);
    const saved = stored?.outputConfig?.[output.id];
    outputConfig[output.id] = {
      visible: typeof saved?.visible === "boolean" ? saved.visible : base.visible,
      color:
        typeof saved?.color === "string" && /^#[0-9a-f]{6}$/i.test(saved.color)
          ? saved.color
          : base.color,
      lineStyle:
        typeof saved?.lineStyle === "string" && STYLE_VALUES.has(saved.lineStyle)
          ? saved.lineStyle
          : base.lineStyle,
      lineWidth:
        typeof saved?.lineWidth === "number" && Number.isFinite(saved.lineWidth)
          ? Math.min(4, Math.max(1, Math.round(saved.lineWidth)))
          : base.lineWidth,
    };
  }
  return {
    enabled: typeof stored?.enabled === "boolean" ? stored.enabled : defaultEnabled,
    values,
    outputConfig,
  };
}

export function saveWasmPreferences(plugins: WasmPluginView[]): void {
  const stored: WasmPreferences = readWasmPreferences();
  for (const plugin of plugins) {
    if (plugin.status !== "ready") continue;
    const outputs: Record<string, Partial<WasmOutputConfig>> = {};
    for (const output of plugin.outputs) {
      const config = plugin.outputConfig[output.id];
      if (config) outputs[output.id] = { ...config };
    }
    stored.plugins[plugin.id] = {
      enabled: plugin.enabled,
      values: [...plugin.values],
      outputConfig: outputs,
    };
  }
  try {
    window.localStorage.setItem(KEY_WASM_INDICATORS, JSON.stringify(stored));
  } catch {
    // private mode / quota: keep the current session working.
  }
}
