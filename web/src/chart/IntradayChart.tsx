import { createEffect, createMemo, createSignal, onCleanup, onMount } from "solid-js";
import {
  AreaSeries,
  ColorType,
  CrosshairMode,
  HistogramSeries,
  LineSeries,
  createChart,
  type IChartApi,
  type ISeriesApi,
  type UTCTimestamp,
} from "lightweight-charts";
import {
  averagePriceLine,
  chinaTimeLabel,
  intradayBars,
  type OHLCV,
} from "../indicators/indicators";
import { chartColors, type Theme } from "./theme";

interface Props {
  bars: OHLCV[];
  symbol: string;
  theme: Theme;
  priceDecimals?: number;
  preClose?: number | null;
  average?: number | null;
}

export default function IntradayChart(props: Props) {
  let container: HTMLDivElement | undefined;
  let chart: IChartApi | undefined;
  let price: ISeriesApi<"Area"> | undefined;
  let average: ISeriesApi<"Line"> | undefined;
  let volume: ISeriesApi<"Histogram"> | undefined;
  let preCloseLine: ReturnType<ISeriesApi<"Area">["createPriceLine"]> | undefined;
  let dataKey = "";
  const [ready, setReady] = createSignal(false);
  const colors = createMemo(() => chartColors(props.theme));
  const decimals = () => Math.max(0, Math.min(6, props.priceDecimals ?? 2));
  const sessionBars = createMemo(() => intradayBars(props.bars, 600));
  const avgValue = createMemo(() => {
    const points = averagePriceLine(sessionBars());
    return points.length ? points[points.length - 1].value : (props.bars.at(-1)?.close ?? 0);
  });

  function applyTheme() {
    if (!chart) return;
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
      timeScale: { borderColor: theme.border },
      localization: { priceFormatter: (value: number) => value.toFixed(decimals()) },
    });
    price?.applyOptions({
      lineColor: theme.ma[0],
      topColor: theme.areaTop,
      bottomColor: theme.areaBottom,
    });
    average?.applyOptions({ color: theme.average });
    volume?.applyOptions({ color: theme.volumeDown });
  }

  function updateData(forceFit = false) {
    if (!chart || !price || !average || !volume) return;
    const bars = sessionBars();
    if (bars.length === 0) {
      price.setData([]);
      average.setData([]);
      volume.setData([]);
      return;
    }
    const nextKey = `${props.symbol}|${bars.length}|${bars[0].time}`;
    const changed = nextKey !== dataKey;
    if (!changed && !forceFit) {
      const last = bars[bars.length - 1];
      price.update({ time: last.time as UTCTimestamp, value: last.close });
      average.update({
        time: last.time as UTCTimestamp,
        value: averagePriceLine(bars).at(-1)?.value ?? last.close,
      });
      volume.update({
        time: last.time as UTCTimestamp,
        value: last.volume,
        color: last.close >= last.open ? colors().volumeUp : colors().volumeDown,
      });
    } else {
      dataKey = nextKey;
      price.setData(
        bars.map((bar) => ({ time: bar.time as UTCTimestamp, value: bar.close })),
      );
      average.setData(
        averagePriceLine(bars).map((point) => ({
          time: point.time as UTCTimestamp,
          value: point.value,
        })),
      );
      volume.setData(
        bars.map((bar) => ({
          time: bar.time as UTCTimestamp,
          value: bar.volume,
          color: bar.close >= bar.open ? colors().volumeUp : colors().volumeDown,
        })),
      );
      if (forceFit || changed) chart.timeScale().fitContent();
    }
    if (preCloseLine) {
      price.removePriceLine(preCloseLine);
      preCloseLine = undefined;
    }
    if (typeof props.preClose === "number" && Number.isFinite(props.preClose) && props.preClose > 0) {
      preCloseLine = price.createPriceLine({
        price: props.preClose,
        color: colors().crosshair,
        lineWidth: 1,
        lineStyle: 2,
        axisLabelVisible: true,
        title: "昨收",
      });
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
      grid: { vertLines: { color: theme.grid }, horzLines: { color: theme.grid } },
      crosshair: {
        mode: CrosshairMode.Normal,
        vertLine: { color: theme.crosshair, labelBackgroundColor: theme.crosshairLabel },
        horzLine: { color: theme.crosshair, labelBackgroundColor: theme.crosshairLabel },
      },
      rightPriceScale: {
        borderColor: theme.border,
        scaleMargins: { top: 0.06, bottom: 0.22 },
      },
      timeScale: {
        borderColor: theme.border,
        timeVisible: true,
        secondsVisible: false,
        rightOffset: 8,
        tickMarkFormatter: (time: unknown) =>
          typeof time === "number" ? chinaTimeLabel(time) : "",
      },
      localization: {
        locale: "zh-CN",
        priceFormatter: (value: number) => value.toFixed(decimals()),
      },
    });
    price = chart.addSeries(AreaSeries, {
      lineColor: theme.ma[0],
      topColor: theme.areaTop,
      bottomColor: theme.areaBottom,
      lineWidth: 2,
      lastValueVisible: true,
      priceLineVisible: false,
      crosshairMarkerVisible: true,
    });
    average = chart.addSeries(LineSeries, {
      color: theme.average,
      lineWidth: 1,
      priceLineVisible: false,
      lastValueVisible: false,
      crosshairMarkerVisible: false,
      title: "均价",
    });
    volume = chart.addSeries(HistogramSeries, {
      priceFormat: { type: "volume" },
      priceScaleId: "",
      color: theme.volumeDown,
      lastValueVisible: false,
      priceLineVisible: false,
    });
    chart.priceScale("").applyOptions({ scaleMargins: { top: 0.82, bottom: 0 } });
    setReady(true);
    updateData(true);
    onCleanup(() => {
      chart?.remove();
      chart = undefined;
      price = undefined;
      average = undefined;
      volume = undefined;
      preCloseLine = undefined;
    });
  });

  createEffect(() => {
    if (!ready()) return;
    colors();
    applyTheme();
  });

  createEffect(() => {
    if (!ready()) return;
    props.bars;
    props.preClose;
    updateData(false);
  });

  return (
    <div class="chart-stage">
      <div class="chart-legend">
        <span class="chart-symbol">{props.symbol} · 分时</span>
        <span>最新 {props.bars.at(-1)?.close?.toFixed(decimals()) ?? "—"}</span>
        <span>
          均价{" "}
          {avgValue().toFixed(decimals())}
        </span>
        <span class="chart-credit">TradingView Lightweight Charts</span>
      </div>
      <div ref={container} class="chart-canvas" aria-label={`${props.symbol} 分时图`} />
    </div>
  );
}
