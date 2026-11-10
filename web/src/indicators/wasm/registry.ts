import { WASM_PLUGIN_REGISTRY } from "./registry.generated";

export interface WasmPluginDescriptor {
  id: string;
  name: string;
  version: string;
  manifestUrl: string;
  defaultEnabled: boolean;
}

/**
 * Auto-generated from web/public/plugins/<id>/manifest.json.
 *
 * Add or remove plugin directories, set `order`/`defaultEnabled` in the
 * manifest, then run `pnpm generate:plugins`.
 */
export const DEFAULT_WASM_PLUGINS: WasmPluginDescriptor[] = WASM_PLUGIN_REGISTRY.map(
  (plugin) => ({ ...plugin }),
);
