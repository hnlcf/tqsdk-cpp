import { For, Show, createEffect, createMemo, createSignal, untrack } from "solid-js";
import type { Quote } from "../api/client";
import { formatPrice, formatVolume, quoteChange } from "../lib/format";
import { instrumentName } from "../lib/instruments";
import type { OHLCV } from "../indicators/indicators";
import type { WasmOutputConfig, WasmPluginView, WasmSeries } from "../indicators/wasm/types";
import KLineChart from "../chart/KLineChart";
import IntradayChart from "../chart/IntradayChart";
import IndicatorSettings from "./IndicatorSettings";
import type { Theme } from "../chart/theme";
import type { KlineAdjust } from "../api/client";
import type { ChartDrawing, DrawMode, NewChartDrawing } from "../chart/drawings";
import { readChartDrawings, saveChartDrawings } from "../lib/storage";

export interface PeriodOption {
  id: string;
  label: string;
  durationNs: number;
}

interface Props {
  symbol: string;
  quote?: Quote;
  bars: OHLCV[];
  period: string;
  periods: PeriodOption[];
  mode: "kline" | "time";
  theme: Theme;
  decimals: number;
  wasmSeries?: WasmSeries[];
  wasmPlugins?: WasmPluginView[];
  onWasmParams?: (id: string, values: number[]) => void;
  onWasmEnabled?: (id: string, enabled: boolean) => void;
  onWasmOutput?: (id: string, outputId: string, patch: Partial<WasmOutputConfig>) => void;
  onPeriod: (period: string) => void;
  onMode: (mode: "kline" | "time") => void;
  continuousMain?: boolean;
  adjust?: KlineAdjust;
  onAdjust?: (mode: KlineAdjust) => void;
}

export default function ChartPanel(props: Props) {
  const [settingsOpen, setSettingsOpen] = createSignal(false);
  const drawingScope = () => `${props.symbol}|${props.period}`;
  const [drawings, setDrawings] = createSignal<ChartDrawing[]>(readChartDrawings(drawingScope()));
  const [drawMode, setDrawMode] = createSignal<DrawMode>("none");
  let drawingScopeKey = drawingScope();
  createEffect(() => {
    const scope = drawingScope();
    if (scope === drawingScopeKey) return;
    drawingScopeKey = scope;
    untrack(() => setDrawings(readChartDrawings(scope)));
    setDrawMode("none");
  });
  const commitDrawings = (next: ChartDrawing[]) => {
    setDrawings(next);
    saveChartDrawings(drawingScope(), next);
  };
  const addDrawing = (drawing: NewChartDrawing) => {
    const id = `drawing-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
    const created: ChartDrawing =
      drawing.kind === "horizontal" ? { ...drawing, id } : { ...drawing, id };
    commitDrawings([...drawings(), created]);
    setDrawMode("none");
  };
  const clearDrawings = () => {
    commitDrawings([]);
    setDrawMode("none");
  };
  const last = createMemo(() => props.bars.at(-1));
  const change = createMemo(() => quoteChange(props.quote));
  const activeWasm = () =>
    (props.wasmPlugins ?? []).filter((plugin) => plugin.enabled && plugin.status === "ready").length;

  return (
    <section class="center-column">
      <div class="chart-header">
        <div class="symbol-title">
          <span class="exchange-badge">{props.symbol.split(".")[0]}</span>
          <div>
            <h1>{props.symbol}</h1>
            <small>{instrumentName(props.symbol)} · {props.quote?.datetime ?? "等待行情"}</small>
          </div>
        </div>
        <div class="quote-block">
          <strong class={change().change >= 0 ? "positive" : "negative"}>
            {formatPrice(props.quote?.last_price, props.decimals)}
          </strong>
          <span class={change().change >= 0 ? "positive" : "negative"}>
            {change().change >= 0 ? "+" : ""}
            {formatPrice(change().change, props.decimals)} / {change().pct.toFixed(2)}%
          </span>
        </div>
        <div class="quote-stats">
          <div>
            <small>买一</small>
            <strong class="positive">{formatPrice(props.quote?.bid_price1, props.decimals)}</strong>
          </div>
          <div>
            <small>卖一</small>
            <strong class="negative">{formatPrice(props.quote?.ask_price1, props.decimals)}</strong>
          </div>
          <div>
            <small>成交量</small>
            <strong>{formatVolume(props.quote?.volume)}</strong>
          </div>
          <div>
            <small>持仓量</small>
            <strong>{formatVolume(props.quote?.open_interest)}</strong>
          </div>
        </div>
      </div>

      <div class="chart-toolbar">
        <div class="period-tabs">
          <button
            type="button"
            class={props.mode === "time" ? "active" : ""}
            onClick={() => props.onMode("time")}
          >
            分时
          </button>
          <For each={props.periods}>
            {(item) => (
              <button
                type="button"
                class={props.mode === "kline" && props.period === item.id ? "active" : ""}
                onClick={() => props.onPeriod(item.id)}
              >
                {item.label}
              </button>
            )}
          </For>
        </div>
        <Show when={props.mode === "kline"}>
          <div class="drawing-tools" aria-label="图表画线工具">
            <button
              type="button"
              class={drawMode() === "horizontal" ? "active" : ""}
              aria-label="画水平线"
              onClick={() => setDrawMode(drawMode() === "horizontal" ? "none" : "horizontal")}
            >
              水平线
            </button>
            <button
              type="button"
              class={drawMode() === "trend" ? "active" : ""}
              aria-label="画趋势线"
              onClick={() => setDrawMode(drawMode() === "trend" ? "none" : "trend")}
            >
              趋势线
            </button>
            <button type="button" aria-label="清除画线" onClick={clearDrawings}>
              清除
            </button>
            <span class="drawing-count">画线 {drawings().length}</span>
          </div>
        </Show>
        <div class="toolbar-tools">
          <Show when={props.continuousMain && props.period === "1d"}>
            <label class="adjust-select">
              <small>主连</small>
              <select
                aria-label="主连复权"
                value={props.adjust ?? "none"}
                onChange={(event) => props.onAdjust?.(event.currentTarget.value as KlineAdjust)}
              >
                <option value="none">原始</option>
                <option value="forward">前复权</option>
                <option value="back">后复权</option>
              </select>
            </label>
          </Show>
          <span class="wasm-toolbar-badge">WASM {activeWasm()}</span>
          <button type="button" class="tool-button indicator-settings-trigger" onClick={() => setSettingsOpen(true)}>
            指标设置
          </button>
        </div>
      </div>

      <div class="chart-shell">
        <Show
          when={props.mode === "kline"}
          fallback={
            <IntradayChart
              bars={props.bars}
              symbol={props.symbol}
              theme={props.theme}
              priceDecimals={props.decimals}
              preClose={props.quote?.pre_close}
              average={props.quote?.average}
            />
          }
        >
          <KLineChart
            bars={props.bars}
            last={last()}
            symbol={props.symbol}
            resetKey={`${props.symbol}|${props.period}|${props.mode}|${props.adjust ?? "none"}`}
            theme={props.theme}
            priceDecimals={props.decimals}
            wasmSeries={props.wasmSeries}
            drawings={drawings()}
            drawMode={drawMode()}
            onDrawing={addDrawing}
          />
        </Show>
      </div>

      <div class="chart-footer">
        <span>O {formatPrice(last()?.open, props.decimals)}</span>
        <span>H {formatPrice(last()?.high, props.decimals)}</span>
        <span>L {formatPrice(last()?.low, props.decimals)}</span>
        <span>C {formatPrice(last()?.close, props.decimals)}</span>
        <span>VOL {formatVolume(last()?.volume)}</span>
        <span class="chart-credit">Lightweight Charts · Apache-2.0</span>
      </div>
      <Show when={settingsOpen()}>
        <div class="indicator-popover-scrim" onClick={() => setSettingsOpen(false)} />
        <IndicatorSettings
          plugins={props.wasmPlugins ?? []}
          onWasmParams={props.onWasmParams}
          onWasmEnabled={props.onWasmEnabled}
          onWasmOutput={props.onWasmOutput}
          onClose={() => setSettingsOpen(false)}
        />
      </Show>
    </section>
  );
}
