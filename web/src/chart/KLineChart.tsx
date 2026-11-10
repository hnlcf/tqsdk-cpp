import { For, createEffect, createMemo, createSignal, onCleanup, onMount } from "solid-js";
import {
  AreaSeries,
  CandlestickSeries,
  ColorType,
  CrosshairMode,
  HistogramSeries,
  LineSeries,
  type CandlestickData,
  createChart,
  createSeriesMarkers,
  type IChartApi,
  type IPriceLine,
  type ISeriesMarkersPluginApi,
  type ISeriesApi,
  type SeriesType,
  type Time,
  type UTCTimestamp,
  TickMarkType,
} from "lightweight-charts";
import type { OHLCV } from "../indicators/indicators";
import { chartColors, type Theme } from "./theme";
import type { WasmSeries } from "../indicators/wasm/types";
import { chinaDateTime, chinaTickMark } from "./time";
import { formatLegendPrice, toCandlestick, toLineData, toLineStyle, toVolumeData } from "./seriesMapping";

interface Props {
  bars: OHLCV[];
  last?: OHLCV;
  symbol: string;
  /// 切换合约/周期时变化；只在该 key 变化时 fitContent。
  resetKey?: string;
  theme: Theme;
  priceDecimals?: number;
  wasmSeries?: WasmSeries[];
}

type AnySeries = ISeriesApi<SeriesType>;

export default function KLineChart(props: Props) {
  let container: HTMLDivElement | undefined;
  let chart: IChartApi | undefined;
  let candle: ISeriesApi<"Candlestick"> | undefined;
  let volume: ISeriesApi<"Histogram"> | undefined;
  let priceLine: IPriceLine | undefined;
  let dataKey = "";
  let appliedResetKey = "";
  let fitPending = true;
  const indicatorSeries = new Map<string, AnySeries>();
  const wasmMarkers = new Map<string, ISeriesMarkersPluginApi<Time>>();
  const [ready, setReady] = createSignal(false);
  const [hover, setHover] = createSignal<OHLCV | undefined>();

  const colors = createMemo(() => chartColors(props.theme));
  const decimals = () => Math.max(0, Math.min(6, props.priceDecimals ?? 2));
  const legendBar = createMemo(() => hover() ?? props.last ?? props.bars.at(-1));

  function ensurePane(index: number) {
    if (!chart) return;
    while (chart.panes().length <= index) chart.addPane();
  }

  function removeExtraPanes() {
    if (!chart) return;
    while (chart.panes().length > 1) {
      chart.removePane(chart.panes().length - 1);
    }
  }

  function clearIndicators() {
    if (!chart) return;
    for (const series of indicatorSeries.values()) {
      try {
        chart.removeSeries(series);
      } catch {
        // Chart may already be disposing; ignoring is safe.
      }
    }
    indicatorSeries.clear();
    for (const markers of wasmMarkers.values()) markers.detach();
    wasmMarkers.clear();
    removeExtraPanes();
  }

  function addLine(
    key: string,
    options: Parameters<IChartApi["addSeries"]>[1],
    paneIndex = 0,
  ): AnySeries | undefined {
    if (!chart) return undefined;
    const series = chart.addSeries(LineSeries, options as never, paneIndex);
    indicatorSeries.set(key, series as AnySeries);
    return series as AnySeries;
  }

  function addWasmSeries() {
    if (!chart || !candle) return;
    const wasmPanes = new Map<string, number>();
    for (const item of props.wasmSeries ?? []) {
      let pane = 0;
      if (item.pane !== "main") {
        const existing = wasmPanes.get(item.pane);
        if (existing !== undefined) {
          pane = existing;
        } else {
          pane = Math.max(1, chart.panes().length);
          ensurePane(pane);
          wasmPanes.set(item.pane, pane);
        }
      }
      const key = `wasm-${item.id}`;
      const lineWidth = Math.min(4, Math.max(1, Math.round(item.lineWidth))) as 1 | 2 | 3 | 4;
      const common = {
        priceLineVisible: false,
        lastValueVisible: false,
        crosshairMarkerVisible: false,
        title: item.title,
      };
      if (item.plot === "markers") {
        wasmMarkers.set(key, createSeriesMarkers(candle, [], {}));
      } else if (item.plot === "band") {
        addLine(`${key}:upper`, {
          ...common,
          color: item.color,
          lineWidth,
          lineStyle: toLineStyle(item.lineStyle),
        }, pane);
        addLine(`${key}:lower`, {
          ...common,
          color: item.color,
          lineWidth,
          lineStyle: toLineStyle(item.lineStyle),
        }, pane);
      } else if (item.plot === "histogram") {
        indicatorSeries.set(
          key,
          chart.addSeries(HistogramSeries, { ...common, color: item.color }, pane) as AnySeries,
        );
      } else if (item.plot === "area") {
        indicatorSeries.set(
          key,
          chart.addSeries(
            AreaSeries,
            {
              ...common,
              lineColor: item.color,
              topColor: `${item.color}40`,
              bottomColor: `${item.color}00`,
              lineWidth,
            },
            pane,
          ) as AnySeries,
        );
      } else {
        addLine(key, {
          ...common,
          color: item.color,
          lineWidth,
          lineStyle: toLineStyle(item.lineStyle),
        }, pane);
      }
    }
  }

  function rebuildIndicators() {
    if (!chart || !candle) return;
    clearIndicators();
    addWasmSeries();
    updateWasmIndicators(props.bars);
  }

  function updateWasmIndicators(bars: OHLCV[]) {
    for (const item of props.wasmSeries ?? []) {
      const key = `wasm-${item.id}`;
      if (item.plot === "band") {
        for (const [suffix, values] of [
          ["upper", item.values],
          ["lower", item.lower ?? new Float64Array()],
        ] as const) {
          const series = indicatorSeries.get(`${key}:${suffix}`);
          if (!series) continue;
          series.setData(toLineData(bars, values));
        }
        continue;
      }
      if (item.plot === "markers") {
        const markers = wasmMarkers.get(key);
        if (!markers) continue;
        markers.setMarkers(
          bars.flatMap((bar, index) => {
            const value = item.values[index];
            if (!Number.isFinite(value)) return [];
            return [{
              time: bar.time as UTCTimestamp,
              position: value >= 0 ? "aboveBar" as const : "belowBar" as const,
              color: item.color,
              shape: "circle" as const,
              text: item.title,
            }];
          }),
        );
        continue;
      }
      const series = indicatorSeries.get(key);
      if (!series) continue;
      series.setData(toLineData(bars, item.values));
    }
  }

  function updateSeriesData(forceFit: boolean) {
    if (!chart || !candle || !volume) return;
    const resetKey = `${props.symbol}|${props.resetKey ?? ""}`;
    if (resetKey !== appliedResetKey) {
      appliedResetKey = resetKey;
      dataKey = "";
      fitPending = true;
      candle.setData([]);
      volume.setData([]);
      if (priceLine) {
        candle.removePriceLine(priceLine);
        priceLine = undefined;
      }
      updateWasmIndicators([]);
    }
    if (forceFit) fitPending = true;

    const bars = props.bars;
    if (bars.length === 0) {
      dataKey = "";
      candle.setData([]);
      volume.setData([]);
      if (priceLine) {
        candle.removePriceLine(priceLine);
        priceLine = undefined;
      }
      updateWasmIndicators([]);
      return;
    }
    const nextKey = `${props.symbol}|${props.resetKey ?? ""}|${bars.length}|${bars[0].time}`;
    if (nextKey !== dataKey) {
      dataKey = nextKey;
      candle.setData(bars.map(toCandlestick));
      volume.setData(toVolumeData(bars, colors().volumeUp, colors().volumeDown));
      if (fitPending) {
        chart.timeScale().fitContent();
        fitPending = false;
      }
    } else {
      const last = props.last ?? bars[bars.length - 1];
      candle.update(toCandlestick(last));
      volume.update(toVolumeData([last], colors().volumeUp, colors().volumeDown)[0]);
    }
    updateWasmIndicators(bars);
    const last = props.last ?? bars[bars.length - 1];
    if (!priceLine) {
      priceLine = candle.createPriceLine({
        price: last.close,
        color: colors().ma[0],
        lineWidth: 1,
        lineStyle: 2,
        axisLabelVisible: true,
        title: "last",
      });
    } else {
      priceLine.applyOptions({ price: last.close });
    }
  }

  onMount(() => {
    if (!container) return;
    const theme = colors();
    chart = createChart(container, {
      autoSize: true,
      layout: {
        background: { type: ColorType.Solid, color: theme.background },
        textColor: theme.text,
        fontFamily: "Inter, SFMono-Regular, Menlo, monospace",
        fontSize: 13,
        attributionLogo: true,
      },
      grid: {
        vertLines: { color: theme.grid },
        horzLines: { color: theme.grid },
      },
      crosshair: {
        mode: CrosshairMode.Normal,
        vertLine: { color: theme.crosshair, labelBackgroundColor: theme.crosshairLabel },
        horzLine: { color: theme.crosshair, labelBackgroundColor: theme.crosshairLabel },
      },
      rightPriceScale: {
        borderColor: theme.border,
        scaleMargins: { top: 0.06, bottom: 0.24 },
      },
      timeScale: {
        borderColor: theme.border,
        timeVisible: true,
        secondsVisible: false,
        rightOffset: 8,
        barSpacing: 7,
        tickMarkFormatter: (time: Time, type: TickMarkType) => chinaTickMark(time, type),
      },
      localization: {
        locale: "zh-CN",
        priceFormatter: (price: number) => price.toFixed(decimals()),
        timeFormatter: (time: Time) =>
          typeof time === "number" ? chinaDateTime(time) : "",
      },
    });

    candle = chart.addSeries(CandlestickSeries, {
      upColor: theme.up,
      downColor: theme.down,
      borderVisible: true,
      borderUpColor: theme.up,
      borderDownColor: theme.down,
      wickUpColor: theme.up,
      wickDownColor: theme.down,
      priceLineVisible: true,
      lastValueVisible: true,
    });
    volume = chart.addSeries(HistogramSeries, {
      priceFormat: { type: "volume" },
      priceScaleId: "",
      color: theme.volumeDown,
      lastValueVisible: false,
      priceLineVisible: false,
    });
    chart.priceScale("").applyOptions({ scaleMargins: { top: 0.8, bottom: 0 } });

    chart.subscribeCrosshairMove((param) => {
      if (!param.point || !param.time || !candle) {
        setHover(undefined);
        return;
      }
      const data = param.seriesData.get(candle) as CandlestickData<UTCTimestamp> | undefined;
      if (!data) {
        setHover(undefined);
        return;
      }
      setHover({
        time: Number(data.time),
        open: data.open,
        high: data.high,
        low: data.low,
        close: data.close,
        volume: props.bars.find((bar) => bar.time === Number(data.time))?.volume ?? 0,
      });
    });

    setReady(true);
    updateSeriesData(true);
    rebuildIndicators();

    onCleanup(() => {
      chart?.remove();
      chart = undefined;
      candle = undefined;
      volume = undefined;
      priceLine = undefined;
    });
  });

  createEffect(() => {
    if (!ready() || !chart || !candle || !volume) return;
    const theme = colors();
    chart.applyOptions({
      layout: {
        background: { type: ColorType.Solid, color: theme.background },
        textColor: theme.text,
      },
      grid: { vertLines: { color: theme.grid }, horzLines: { color: theme.grid } },
      crosshair: {
        mode: CrosshairMode.Normal,
        vertLine: { color: theme.crosshair, labelBackgroundColor: theme.crosshairLabel },
        horzLine: { color: theme.crosshair, labelBackgroundColor: theme.crosshairLabel },
      },
      rightPriceScale: { borderColor: theme.border },
      timeScale: {
        borderColor: theme.border,
        tickMarkFormatter: (time: Time, type: TickMarkType) => chinaTickMark(time, type),
      },
      localization: {
        priceFormatter: (price: number) => price.toFixed(decimals()),
        timeFormatter: (time: Time) =>
          typeof time === "number" ? chinaDateTime(time) : "",
      },
    });
    candle.applyOptions({
      upColor: theme.up,
      downColor: theme.down,
      borderUpColor: theme.up,
      borderDownColor: theme.down,
      wickUpColor: theme.up,
      wickDownColor: theme.down,
    });
    volume.applyOptions({ color: theme.volumeDown });
    rebuildIndicators();
  });

  createEffect(() => {
    if (!ready() || !chart) return;
    // Rebuild only when the symbol/reset key, WASM series definition or theme
    // changes; incremental price updates must not recreate series.
    props.symbol;
    props.resetKey;
    props.wasmSeries;
    colors();
    rebuildIndicators();
  });

  createEffect(() => {
    if (!ready()) return;
    props.symbol;
    props.resetKey;
    props.bars;
    props.last;
    // 实时更新不能 fitContent；只有 symbol/resetKey 切换或首次数据才重置缩放。
    updateSeriesData(false);
  });

  return (
    <div class="chart-stage">
      <div class="chart-legend">
        <span class="chart-symbol">{props.symbol}</span>
        <span>
          O {formatLegendPrice(legendBar()?.open, decimals())} H{" "}
          {formatLegendPrice(legendBar()?.high, decimals())} L{" "}
          {formatLegendPrice(legendBar()?.low, decimals())} C{" "}
          {formatLegendPrice(legendBar()?.close, decimals())}
        </span>
        <span>VOL {legendBar()?.volume?.toLocaleString("zh-CN") ?? "—"}</span>
        <For each={props.wasmSeries ?? []}>
          {(item) => (
            <span class="chart-wasm" title="WASM indicator" style={{ color: item.color }}>
              {item.title}
            </span>
          )}
        </For>
        <span class="chart-credit">TradingView Lightweight Charts</span>
      </div>
      <div ref={container} class="chart-canvas" aria-label={`${props.symbol} K线图`} />
    </div>
  );
}
