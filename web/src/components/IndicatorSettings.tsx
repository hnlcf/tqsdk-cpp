import { For, Show, createMemo, createSignal } from "solid-js";
import {
  resolveOutputTitle,
  type WasmOutputConfig,
  type WasmOutputSpec,
  type WasmPluginView,
} from "../indicators/wasm/types";

interface Props {
  plugins: WasmPluginView[];
  onWasmParams?: (id: string, values: number[]) => void;
  onWasmEnabled?: (id: string, enabled: boolean) => void;
  onWasmOutput?: (
    id: string,
    outputId: string,
    patch: Partial<WasmOutputConfig>,
  ) => void;
  onClose: () => void;
}

export default function IndicatorSettings(props: Props) {
  const [selectedId, setSelectedId] = createSignal<string>();
  const selected = createMemo(() => props.plugins.find((plugin) => plugin.id === selectedId()));

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

  function outputConfig(plugin: WasmPluginView, output: WasmOutputSpec): WasmOutputConfig {
    return (
      plugin.outputConfig[output.id] ?? {
        visible: output.defaultVisible !== false,
        color: output.color,
        lineStyle: "solid",
        lineWidth: output.lineWidth ?? 1,
      }
    );
  }

  function patchOutput(
    plugin: WasmPluginView,
    output: WasmOutputSpec,
    patch: Partial<WasmOutputConfig>,
  ) {
    props.onWasmOutput?.(plugin.id, output.id, patch);
  }

  function paramIndex(plugin: WasmPluginView, output: WasmOutputSpec): number {
    if (!output.periodParam) return -1;
    return plugin.params.findIndex((param) => param.id === output.periodParam);
  }

  return (
    <section class="indicator-popover" role="dialog" aria-label="指标设置">
      <div class="indicator-popover-head">
        <Show
          when={selected()}
          fallback={
            <div>
              <span class="eyebrow">WASM INDICATORS</span>
              <h2>指标</h2>
            </div>
          }
        >
          {(plugin) => (
            <div class="indicator-detail-heading">
              <button
                type="button"
                class="icon-button indicator-back"
                title="返回指标列表"
                onClick={() => setSelectedId(undefined)}
              >
                ‹
              </button>
              <div>
                <span class="eyebrow">WASM INDICATOR</span>
                <h2>{plugin().name}</h2>
              </div>
            </div>
          )}
        </Show>
        <button type="button" class="icon-button" onClick={props.onClose} title="关闭">
          ×
        </button>
      </div>

      <div class="indicator-popover-body">
        <Show
          when={selected()}
          fallback={
            <div class="indicator-picker-list">
              <small class="indicator-hint">
                Worker + WebAssembly 沙箱 · 点击指标进入参数与输出配置
              </small>
              <Show when={props.plugins.length === 0}>
                <div class="empty-state small">正在加载 WASM 指标...</div>
              </Show>
              <For each={props.plugins}>
                {(plugin) => (
                  <div
                    class="indicator-picker-row"
                    classList={{ active: plugin.enabled }}
                  >
                    <label class="indicator-picker-toggle">
                      <input
                        type="checkbox"
                        checked={plugin.enabled}
                        disabled={plugin.status !== "ready"}
                        onChange={(event) =>
                          props.onWasmEnabled?.(plugin.id, event.currentTarget.checked)
                        }
                      />
                    </label>
                    <button
                      type="button"
                      class="indicator-picker-open"
                      onClick={() => setSelectedId(plugin.id)}
                    >
                      <span class="indicator-picker-name">
                        <strong>{plugin.name}</strong>
                        <small>
                          v{plugin.version || "—"} ·{" "}
                          {plugin.status === "ready"
                            ? plugin.dynamicLines
                              ? `${plugin.outputs.filter((output) => outputConfig(plugin, output).visible).length} 条线`
                              : "已加载"
                            : plugin.status === "error"
                              ? "加载失败"
                              : "加载中"}
                        </small>
                      </span>
                      <span class="indicator-picker-arrow">›</span>
                    </button>
                  </div>
                )}
              </For>
            </div>
          }
        >
          {(plugin) => (
            <div class="indicator-detail">
              <div class="indicator-detail-meta">
                <span class="wasm-badge">v{plugin().version || "—"}</span>
                <label class="indicator-inline-toggle">
                  <input
                    type="checkbox"
                    checked={plugin().enabled}
                    disabled={plugin().status !== "ready"}
                    onClick={(event) => {
                      event.stopPropagation();
                      props.onWasmEnabled?.(plugin().id, event.currentTarget.checked);
                    }}
                  />
                  启用
                </label>
              </div>
              <Show when={plugin().status === "loading"}>
                <small class="indicator-hint">正在加载插件...</small>
              </Show>
              <Show when={plugin().status === "error"}>
                <small class="indicator-hint negative">
                  加载失败：{plugin().error ?? "unknown error"}
                </small>
              </Show>

              <Show
                when={plugin().dynamicLines}
                fallback={
                  <>
                    <div class="indicator-section-title">参数</div>
                    <For each={plugin().params}>
                      {(param, index) => (
                        <div class="indicator-param-row">
                          <label>
                            {param.label}
                            <input
                              type="number"
                              min={param.min}
                              max={param.max}
                              step={param.type === "int" ? 1 : 0.1}
                              value={plugin().values[index()] ?? param.default}
                              disabled={plugin().status !== "ready"}
                              onChange={(event) =>
                                setParam(plugin(), index(), event.currentTarget.value)
                              }
                            />
                          </label>
                        </div>
                      )}
                    </For>

                    <div class="indicator-section-title">输出配置</div>
                    <For each={plugin().outputs}>
                      {(output) => {
                        const config = () => outputConfig(plugin(), output);
                        return (
                          <div class="output-config-row">
                            <label class="indicator-inline-toggle">
                              <input
                                type="checkbox"
                                checked={config().visible}
                                disabled={plugin().status !== "ready"}
                                onClick={(event) =>
                                  patchOutput(plugin(), output, {
                                    visible: event.currentTarget.checked,
                                  })
                                }
                              />
                              <strong>
                                {resolveOutputTitle(
                                  output.title,
                                  { params: plugin().params },
                                  plugin().values,
                                )}
                              </strong>
                            </label>
                            <label>
                              颜色
                              <input
                                type="color"
                                value={config().color}
                                onChange={(event) =>
                                  patchOutput(plugin(), output, {
                                    color: event.currentTarget.value,
                                  })
                                }
                              />
                            </label>
                            <label>
                              线型
                              <select
                                value={config().lineStyle}
                                onChange={(event) =>
                                  patchOutput(plugin(), output, {
                                    lineStyle: event.currentTarget.value as WasmOutputConfig["lineStyle"],
                                  })
                                }
                              >
                                <option value="solid">实线</option>
                                <option value="dashed">虚线</option>
                                <option value="dotted">点线</option>
                              </select>
                            </label>
                            <label>
                              宽度
                              <input
                                type="number"
                                min="1"
                                max="4"
                                step="1"
                                value={config().lineWidth}
                                onChange={(event) =>
                                  patchOutput(plugin(), output, {
                                    lineWidth: Math.min(
                                      4,
                                      Math.max(1, Number(event.currentTarget.value) || 1),
                                    ),
                                  })
                                }
                              />
                            </label>
                          </div>
                        );
                      }}
                    </For>
                  </>
                }
              >
                <div class="indicator-section-title">
                  均线
                  <span class="indicator-hint">可添加/移除，最多 {plugin().outputs.length} 条</span>
                </div>
                <For
                  each={plugin().outputs.filter(
                    (output) => outputConfig(plugin(), output).visible,
                  )}
                >
                  {(output) => {
                    const config = () => outputConfig(plugin(), output);
                    const pIndex = () => paramIndex(plugin(), output);
                    const pSpec = () => (pIndex() >= 0 ? plugin().params[pIndex()] : undefined);
                    return (
                      <div class="indicator-line-config">
                        <div class="indicator-line-title">
                          <strong>
                            {resolveOutputTitle(
                              output.title,
                              { params: plugin().params },
                              plugin().values,
                            )}
                          </strong>
                          <button
                            type="button"
                            class="table-action danger"
                            onClick={() =>
                              patchOutput(plugin(), output, { visible: false })
                            }
                          >
                            移除
                          </button>
                        </div>
                        <div class="indicator-param-row">
                          <Show when={pSpec()}>
                            {(spec) => (
                              <label>
                                周期
                                <input
                                  type="number"
                                  min={spec().min}
                                  max={spec().max}
                                  step="1"
                                  value={plugin().values[pIndex()] ?? spec().default}
                                  onChange={(event) =>
                                    setParam(plugin(), pIndex(), event.currentTarget.value)
                                  }
                                />
                              </label>
                            )}
                          </Show>
                          <label>
                            颜色
                            <input
                              type="color"
                              value={config().color}
                              onChange={(event) =>
                                patchOutput(plugin(), output, {
                                  color: event.currentTarget.value,
                                })
                              }
                            />
                          </label>
                          <label>
                            线型
                            <select
                              value={config().lineStyle}
                              onChange={(event) =>
                                patchOutput(plugin(), output, {
                                  lineStyle: event.currentTarget.value as WasmOutputConfig["lineStyle"],
                                })
                              }
                            >
                              <option value="solid">实线</option>
                              <option value="dashed">虚线</option>
                              <option value="dotted">点线</option>
                            </select>
                          </label>
                          <label>
                            宽度
                            <input
                              type="number"
                              min="1"
                              max="4"
                              step="1"
                              value={config().lineWidth}
                              onChange={(event) =>
                                patchOutput(plugin(), output, {
                                  lineWidth: Math.min(
                                    4,
                                    Math.max(1, Number(event.currentTarget.value) || 1),
                                  ),
                                })
                              }
                            />
                          </label>
                        </div>
                      </div>
                    );
                  }}
                </For>
                <Show
                  when={plugin().outputs.some(
                    (output) => !outputConfig(plugin(), output).visible,
                  )}
                >
                  <button
                    type="button"
                    class="table-action indicator-add-line"
                    onClick={() => {
                      const hidden = plugin().outputs.find(
                        (output) => !outputConfig(plugin(), output).visible,
                      );
                      if (hidden) patchOutput(plugin(), hidden, { visible: true });
                    }}
                  >
                    + 添加均线
                  </button>
                </Show>
              </Show>
            </div>
          )}
        </Show>
      </div>
    </section>
  );
}
