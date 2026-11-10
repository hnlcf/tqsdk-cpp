import { For, Show } from "solid-js";
import type { WasmPluginView } from "../indicators/wasm/types";

interface Props {
  plugins: WasmPluginView[];
  onWasmParams?: (id: string, values: number[]) => void;
  onWasmEnabled?: (id: string, enabled: boolean) => void;
  onClose: () => void;
}

export default function IndicatorSettings(props: Props) {
  function setParam(plugin: WasmPluginView, index: number, raw: string) {
    const value = Number(raw);
    if (!Number.isFinite(value)) return;
    const spec = plugin.params[index];
    if (!spec) return;
    const clamped = Math.min(
      spec.max ?? Number.POSITIVE_INFINITY,
      Math.max(spec.min ?? Number.NEGATIVE_INFINITY, value),
    );
    const values = [...plugin.values];
    while (values.length < plugin.params.length) {
      values.push(plugin.params[values.length]?.default ?? 0);
    }
    values[index] = spec.type === "int" ? Math.round(clamped) : clamped;
    props.onWasmParams?.(plugin.id, values);
  }

  return (
    <div class="indicator-settings-backdrop" onClick={props.onClose}>
      <section
        class="indicator-settings"
        role="dialog"
        aria-label="指标设置"
        onClick={(event) => event.stopPropagation()}
      >
        <div class="indicator-settings-head">
          <div>
            <span class="eyebrow">WASM INDICATORS</span>
            <h2>指标设置</h2>
          </div>
          <button type="button" class="icon-button" onClick={props.onClose} title="关闭">
            ×
          </button>
        </div>

        <div class="indicator-settings-body">
          <small class="indicator-hint">
            Worker + WebAssembly 沙箱 · 无 host imports · 只读历史 OHLCV
          </small>
          <Show when={props.plugins.length === 0}>
            <div class="empty-state small">正在加载 WASM 指标...</div>
          </Show>
          <For each={props.plugins}>
            {(plugin) => (
              <div class="indicator-block wasm-indicator-block">
                <div class="indicator-block-title">
                  <strong>{plugin.name || plugin.id}</strong>
                  <span class="wasm-badge">
                    {plugin.version ? `v${plugin.version}` : plugin.status}
                  </span>
                </div>
                <label class="indicator-toggle">
                  <input
                    type="checkbox"
                    checked={plugin.enabled}
                    disabled={plugin.status !== "ready"}
                    onChange={(event) =>
                      props.onWasmEnabled?.(plugin.id, event.currentTarget.checked)
                    }
                  />
                  <strong>启用</strong>
                </label>
                <Show when={plugin.status === "loading"}>
                  <small class="indicator-hint">正在加载插件...</small>
                </Show>
                <Show when={plugin.status === "error"}>
                  <small class="indicator-hint negative">
                    加载失败：{plugin.error ?? "unknown error"}
                  </small>
                </Show>
                <For each={plugin.params}>
                  {(param, index) => (
                    <div class="indicator-param-row">
                      <label>
                        {param.label}
                        <input
                          type="number"
                          min={param.min}
                          max={param.max}
                          step={param.type === "int" ? 1 : 0.1}
                          value={plugin.values[index()] ?? param.default}
                          disabled={plugin.status !== "ready"}
                          onChange={(event) =>
                            setParam(plugin, index(), event.currentTarget.value)
                          }
                        />
                      </label>
                    </div>
                  )}
                </For>
              </div>
            )}
          </For>
        </div>

        <div class="indicator-settings-foot">
          <span class="indicator-hint">参数修改后自动重算</span>
          <button type="button" class="submit-order buy" onClick={props.onClose}>
            完成
          </button>
        </div>
      </section>
    </div>
  );
}
