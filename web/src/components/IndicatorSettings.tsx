import { For, Show } from "solid-js";
import {
  DEFAULT_INDICATORS,
  type IndicatorConfig,
  type IndicatorColors,
} from "../indicators/indicators";
import { chartColors, type Theme } from "../chart/theme";

interface Props {
  config: IndicatorConfig;
  theme: Theme;
  onChange: (next: IndicatorConfig) => void;
  onClose: () => void;
}

function colorValue(value: string | undefined, fallback: string): string {
  if (value && /^#[0-9a-f]{6}$/i.test(value)) return value;
  if (/^#[0-9a-f]{6}$/i.test(fallback)) return fallback;
  return "#53b7ff";
}

export default function IndicatorSettings(props: Props) {
  const colors = () => chartColors(props.theme);
  const patch = (next: Partial<IndicatorConfig>) => props.onChange({ ...props.config, ...next });
  const patchColors = (key: "bollColors" | "macdColors" | "rsiColors", next: IndicatorColors) =>
    patch({ [key]: { ...(props.config[key] ?? {}), ...next } } as Partial<IndicatorConfig>);

  function setMaPeriod(index: number, value: string) {
    const period = Number(value);
    if (!Number.isInteger(period) || period <= 0) return;
    const periods = props.config.maPeriods.slice();
    periods[index] = period;
    patch({ maPeriods: [...new Set(periods)] });
  }

  function addMa() {
    const next = (props.config.maPeriods.at(-1) ?? 10) + 10;
    patch({ maPeriods: [...props.config.maPeriods, next] });
  }

  function removeMa(index: number) {
    patch({ maPeriods: props.config.maPeriods.filter((_, current) => current !== index) });
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
            <span class="eyebrow">INDICATORS</span>
            <h2>指标设置</h2>
          </div>
          <button type="button" class="icon-button" onClick={props.onClose} title="关闭">
            ×
          </button>
        </div>

        <div class="indicator-settings-body">
          <div class="indicator-block">
            <div class="indicator-block-title">
              <strong>MA</strong>
              <button type="button" class="table-action" onClick={addMa}>
                + 加一条
              </button>
            </div>
            <Show when={props.config.maPeriods.length === 0}>
              <small class="indicator-hint">当前没有 MA 线。</small>
            </Show>
            <For each={props.config.maPeriods}>
              {(period, index) => (
                <div class="indicator-param-row">
                  <label>
                    周期
                    <input
                      type="number"
                      min="1"
                      value={period}
                      onChange={(event) => setMaPeriod(index(), event.currentTarget.value)}
                    />
                  </label>
                  <label>
                    颜色
                    <input
                      type="color"
                      value={colorValue(
                        props.config.maColors?.[index()],
                        colors().ma[index() % colors().ma.length],
                      )}
                      onChange={(event) => {
                        const next = [...(props.config.maColors ?? [])];
                        next[index()] = event.currentTarget.value;
                        patch({ maColors: next });
                      }}
                    />
                  </label>
                  <button type="button" class="table-action danger" onClick={() => removeMa(index())}>
                    删除
                  </button>
                </div>
              )}
            </For>
          </div>

          <div class="indicator-block">
            <label class="indicator-toggle">
              <input
                type="checkbox"
                checked={props.config.boll}
                onChange={(event) => patch({ boll: event.currentTarget.checked })}
              />
              <strong>BOLL</strong>
            </label>
            <div class="indicator-param-row">
              <label>
                周期
                <input
                  type="number"
                  min="2"
                  value={props.config.bollPeriod}
                  onChange={(event) =>
                    patch({ bollPeriod: Math.max(2, Number(event.currentTarget.value)) })
                  }
                />
              </label>
              <label>
                倍数
                <input
                  type="number"
                  min="0.1"
                  step="0.1"
                  value={props.config.bollMultiplier}
                  onChange={(event) =>
                    patch({ bollMultiplier: Math.max(0.1, Number(event.currentTarget.value)) })
                  }
                />
              </label>
            </div>
            <div class="indicator-color-row">
              <label>
                上轨
                <input
                  type="color"
                  value={colorValue(props.config.bollColors?.bollUpper, colors().bollUpper)}
                  onChange={(event) => patchColors("bollColors", { bollUpper: event.currentTarget.value })}
                />
              </label>
              <label>
                中轨
                <input
                  type="color"
                  value={colorValue(props.config.bollColors?.bollMiddle, colors().bollMiddle)}
                  onChange={(event) =>
                    patchColors("bollColors", { bollMiddle: event.currentTarget.value })
                  }
                />
              </label>
              <label>
                下轨
                <input
                  type="color"
                  value={colorValue(props.config.bollColors?.bollLower, colors().bollLower)}
                  onChange={(event) => patchColors("bollColors", { bollLower: event.currentTarget.value })}
                />
              </label>
            </div>
          </div>

          <div class="indicator-block">
            <label class="indicator-toggle">
              <input
                type="checkbox"
                checked={props.config.macd}
                onChange={(event) => patch({ macd: event.currentTarget.checked })}
              />
              <strong>MACD</strong>
            </label>
            <div class="indicator-param-row">
              <label>
                快线
                <input
                  type="number"
                  min="1"
                  value={props.config.macdFast}
                  onChange={(event) =>
                    patch({ macdFast: Math.max(1, Number(event.currentTarget.value)) })
                  }
                />
              </label>
              <label>
                慢线
                <input
                  type="number"
                  min="1"
                  value={props.config.macdSlow}
                  onChange={(event) =>
                    patch({ macdSlow: Math.max(1, Number(event.currentTarget.value)) })
                  }
                />
              </label>
              <label>
                信号
                <input
                  type="number"
                  min="1"
                  value={props.config.macdSignal}
                  onChange={(event) =>
                    patch({ macdSignal: Math.max(1, Number(event.currentTarget.value)) })
                  }
                />
              </label>
            </div>
            <div class="indicator-color-row">
              <label>
                DIF
                <input
                  type="color"
                  value={colorValue(props.config.macdColors?.macdDif, colors().macdDif)}
                  onChange={(event) => patchColors("macdColors", { macdDif: event.currentTarget.value })}
                />
              </label>
              <label>
                DEA
                <input
                  type="color"
                  value={colorValue(props.config.macdColors?.macdDea, colors().macdDea)}
                  onChange={(event) => patchColors("macdColors", { macdDea: event.currentTarget.value })}
                />
              </label>
              <label>
                柱涨
                <input
                  type="color"
                  value={colorValue(props.config.macdColors?.macdHistUp, colors().positive)}
                  onChange={(event) =>
                    patchColors("macdColors", { macdHistUp: event.currentTarget.value })
                  }
                />
              </label>
              <label>
                柱跌
                <input
                  type="color"
                  value={colorValue(props.config.macdColors?.macdHistDown, colors().negative)}
                  onChange={(event) =>
                    patchColors("macdColors", { macdHistDown: event.currentTarget.value })
                  }
                />
              </label>
            </div>
          </div>

          <div class="indicator-block">
            <label class="indicator-toggle">
              <input
                type="checkbox"
                checked={props.config.rsi}
                onChange={(event) => patch({ rsi: event.currentTarget.checked })}
              />
              <strong>RSI</strong>
            </label>
            <div class="indicator-param-row">
              <label>
                周期
                <input
                  type="number"
                  min="2"
                  value={props.config.rsiPeriod}
                  onChange={(event) =>
                    patch({ rsiPeriod: Math.max(2, Number(event.currentTarget.value)) })
                  }
                />
              </label>
              <label>
                颜色
                <input
                  type="color"
                  value={colorValue(props.config.rsiColors?.rsi, colors().average)}
                  onChange={(event) => patchColors("rsiColors", { rsi: event.currentTarget.value })}
                />
              </label>
            </div>
          </div>
        </div>

        <div class="indicator-settings-foot">
          <button type="button" class="table-action" onClick={() => props.onChange({ ...DEFAULT_INDICATORS })}>
            恢复默认
          </button>
          <button type="button" class="submit-order buy" onClick={props.onClose}>
            完成
          </button>
        </div>
      </section>
    </div>
  );
}
