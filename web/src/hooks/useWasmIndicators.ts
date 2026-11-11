import {
  createEffect,
  createSignal,
  onCleanup,
  onMount,
  untrack,
  type Accessor,
} from "solid-js";
import type { OHLCV } from "../indicators/indicators";
import { WasmIndicatorHost } from "../indicators/wasm/host";
import { DEFAULT_WASM_PLUGINS } from "../indicators/wasm/registry";
import {
  applyWasmPreference,
  readWasmPreferences,
  saveWasmPreferences,
} from "../indicators/wasm/preferences";
import {
  seriesFromManifest,
  type WasmManifest,
  type WasmOutputConfig,
  type WasmPluginView,
  type WasmSeries,
} from "../indicators/wasm/types";

/**
 * Owns the WASM plugin lifecycle, parameter/output mutations and the
 * debounced compute cache. `chartScope` must identify the current
 * symbol+period so a cached result cannot leak across charts.
 */
export function useWasmIndicators(bars: Accessor<OHLCV[]>, chartScope: Accessor<string>) {
  const [series, setSeries] = createSignal<WasmSeries[]>([]);
  const [plugins, setPlugins] = createSignal<WasmPluginView[]>([]);
  const hosts = new Map<string, WasmIndicatorHost>();
  const cache = new Map<
    string,
    { key: string; outputs: Float64Array[]; manifest: WasmManifest }
  >();
  let timer: number | undefined;
  let generation = 0;

  function patch(id: string, next: Partial<WasmPluginView>) {
    setPlugins((previous) =>
      previous.map((plugin) => (plugin.id === id ? { ...plugin, ...next } : plugin)),
    );
  }

  function updateParams(id: string, values: number[]) {
    patch(id, { values });
  }

  function setEnabled(id: string, enabled: boolean) {
    patch(id, { enabled });
  }

  function updateOutput(id: string, outputId: string, next: Partial<WasmOutputConfig>) {
    setPlugins((previous) =>
      previous.map((plugin) =>
        plugin.id === id
          ? {
              ...plugin,
              outputConfig: {
                ...plugin.outputConfig,
                [outputId]: { ...plugin.outputConfig[outputId], ...next },
              },
            }
          : plugin,
      ),
    );
  }

  function schedule() {
    generation += 1;
    const scheduled = generation;
    if (timer !== undefined) {
      window.clearTimeout(timer);
      timer = undefined;
    }
    const currentBars = bars();
    const active = plugins().filter(
      (plugin) =>
        plugin.enabled &&
        plugin.status === "ready" &&
        plugin.outputs.some((output) => plugin.outputConfig[output.id]?.visible !== false),
    );
    if (currentBars.length === 0 || active.length === 0) {
      setSeries([]);
      return;
    }
    const snapshot = active.map((plugin) => ({ ...plugin, values: [...plugin.values] }));
    timer = window.setTimeout(() => {
      timer = undefined;
      if (scheduled !== generation) return;
      const latest = bars();
      if (latest.length === 0) return;
      const barsKey = [
        latest.length,
        latest[0]?.time ?? 0,
        latest.at(-1)?.time ?? 0,
        latest.at(-1)?.close ?? 0,
        latest.at(-1)?.high ?? 0,
        latest.at(-1)?.low ?? 0,
        latest.at(-1)?.volume ?? 0,
      ].join(":");
      const scope = chartScope();
      void Promise.all(
        snapshot.map(async (plugin) => {
          const host = hosts.get(plugin.id);
          const manifest = host?.manifest;
          if (!host || !manifest) return [] as WasmSeries[];
          const values =
            plugin.values.length === manifest.params.length
              ? plugin.values
              : manifest.params.map((param) => param.default);
          const key = `${scope}|${barsKey}|${values.join(",")}`;
          const cached = cache.get(plugin.id);
          let outputs: Float64Array[];
          try {
            if (cached?.key === key && cached.manifest === manifest) {
              outputs = cached.outputs;
            } else {
              outputs = await host.compute(latest, values);
              cache.set(plugin.id, { key, outputs, manifest });
            }
            return seriesFromManifest(manifest, outputs, values, plugin.outputConfig).map(
              (item) => ({
                ...item,
                id: `${manifest.id}:${item.id}`,
              }),
            );
          } catch {
            cache.delete(plugin.id);
            return [] as WasmSeries[];
          }
        }),
      ).then((results) => {
        // Drop results from an older debounce/compute; a newer bars/params
        // generation must win even if an older plugin worker finishes later.
        if (scheduled === generation) setSeries(results.flat());
      });
    }, 120);
  }

  onMount(() => {
    const preferences = readWasmPreferences();
    for (const descriptor of DEFAULT_WASM_PLUGINS) {
      const host = new WasmIndicatorHost();
      hosts.set(descriptor.id, host);
      setPlugins((previous) => [
        ...previous,
        {
          id: descriptor.id,
          name: descriptor.name,
          version: descriptor.version,
          enabled: descriptor.defaultEnabled,
          status: "loading",
          params: [],
          values: [],
          outputs: [],
          outputConfig: {},
        },
      ]);
      void host
        .load(descriptor.manifestUrl)
        .then((manifest) => {
          const applied = applyWasmPreference(
            manifest,
            descriptor.defaultEnabled,
            preferences.plugins[descriptor.id],
          );
          patch(descriptor.id, {
            name: manifest.name,
            version: manifest.version,
            status: "ready",
            params: manifest.params,
            values: applied.values,
            outputs: manifest.outputs,
            outputConfig: applied.outputConfig,
            dynamicLines: manifest.dynamicLines === true,
            enabled: applied.enabled,
          });
        })
        .catch((error) => {
          patch(descriptor.id, {
            status: "error",
            error: error instanceof Error ? error.message : String(error),
          });
        });
    }
  });

  createEffect(() => {
    bars();
    plugins();
    untrack(schedule);
  });

  createEffect(() => {
    const ready = plugins().filter((plugin) => plugin.status === "ready");
    if (ready.length > 0) saveWasmPreferences(ready);
  });

  onCleanup(() => {
    if (timer !== undefined) {
      window.clearTimeout(timer);
      timer = undefined;
    }
    for (const host of hosts.values()) host.close();
    hosts.clear();
  });

  return {
    series,
    plugins,
    updateParams,
    setEnabled,
    updateOutput,
  };
}
