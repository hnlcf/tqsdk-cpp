import { createEffect, onCleanup, onMount } from "solid-js";
import {
  CandlestickSeries,
  ColorType,
  CrosshairMode,
  HistogramSeries,
  createChart,
  type IChartApi,
  type IPriceLine,
  type ISeriesApi,
  type UTCTimestamp,
} from "lightweight-charts";
import type { Bar } from "../data/mock";

interface Props {
  bars: Bar[];
  last?: Bar;
  symbol: string;
  theme: "dark" | "light";
}

function themeColors(theme: Props["theme"]) {
  return theme === "light"
    ? {
        background: "#f6f8fc",
        text: "#4d6077",
        grid: "#e5ebf3",
        border: "#d5dfeb",
        crosshair: "#7e91a8",
        crosshairLabel: "#dce6f2",
        volume: "#9db1c7",
      }
    : {
        background: "#0b1016",
        text: "#9aa7b8",
        grid: "#151f2b",
        border: "#223043",
        crosshair: "#52657a",
        crosshairLabel: "#223043",
        volume: "#38506a",
      };
}

export default function KLineChart(props: Props) {
  let container: HTMLDivElement | undefined;
  let chart: IChartApi | undefined;
  let candle: ISeriesApi<"Candlestick"> | undefined;
  let volume: ISeriesApi<"Histogram"> | undefined;
  let priceLine: IPriceLine | undefined;
  let dataKey = "";

  onMount(() => {
    if (!container) return;

    const colors = themeColors(props.theme);
    chart = createChart(container, {
      autoSize: true,
      layout: {
        background: { type: ColorType.Solid, color: colors.background },
        textColor: colors.text,
        fontFamily: "Inter, SFMono-Regular, Menlo, monospace",
        attributionLogo: true,
      },
      grid: {
        vertLines: { color: colors.grid },
        horzLines: { color: colors.grid },
      },
      crosshair: {
        mode: CrosshairMode.Normal,
        vertLine: { color: colors.crosshair, labelBackgroundColor: colors.crosshairLabel },
        horzLine: { color: colors.crosshair, labelBackgroundColor: colors.crosshairLabel },
      },
      rightPriceScale: {
        borderColor: colors.border,
        scaleMargins: { top: 0.08, bottom: 0.28 },
      },
      timeScale: {
        borderColor: colors.border,
        timeVisible: true,
        secondsVisible: false,
        rightOffset: 4,
        barSpacing: 7,
      },
      localization: {
        locale: "zh-CN",
        priceFormatter: (price: number) => price.toFixed(0),
      },
    });

    candle = chart.addSeries(CandlestickSeries, {
      upColor: "#16c784",
      downColor: "#ea3943",
      borderVisible: false,
      wickUpColor: "#16c784",
      wickDownColor: "#ea3943",
      priceLineVisible: true,
      lastValueVisible: true,
    });

    volume = chart.addSeries(HistogramSeries, {
      priceFormat: { type: "volume" },
      priceScaleId: "",
      color: colors.volume,
      lastValueVisible: false,
      priceLineVisible: false,
    });
    chart.priceScale("").applyOptions({
      scaleMargins: { top: 0.78, bottom: 0 },
    });

    candle.setData(
      props.bars.map((bar) => ({
        time: bar.time as UTCTimestamp,
        open: bar.open,
        high: bar.high,
        low: bar.low,
        close: bar.close,
      })),
    );
    volume.setData(
      props.bars.map((bar) => ({
        time: bar.time as UTCTimestamp,
        value: bar.volume,
        color: bar.close >= bar.open ? "#16c78455" : "#ea394355",
      })),
    );
    chart.timeScale().fitContent();

    if (props.last) {
      upsertLast(props.last);
    }

    const resize = new ResizeObserver(() => {
      if (container && chart) {
        chart.applyOptions({
          width: container.clientWidth,
          height: container.clientHeight,
        });
      }
    });
    resize.observe(container);

    onCleanup(() => {
      resize.disconnect();
      chart?.remove();
      chart = undefined;
      candle = undefined;
      volume = undefined;
      priceLine = undefined;
    });
  });

  createEffect(() => {
    const bars = props.bars;
    if (!bars.length || !candle || !volume) return;
    const nextKey = `${props.symbol}:${bars.length}:${bars[0].time}:${bars[bars.length - 1].time}`;
    if (nextKey === dataKey) return;
    dataKey = nextKey;
    candle.setData(
      bars.map((bar) => ({
        time: bar.time as UTCTimestamp,
        open: bar.open,
        high: bar.high,
        low: bar.low,
        close: bar.close,
      })),
    );
    volume.setData(
      bars.map((bar) => ({
        time: bar.time as UTCTimestamp,
        value: bar.volume,
        color: bar.close >= bar.open ? "#16c78455" : "#ea394355",
      })),
    );
    chart?.timeScale().fitContent();
  });

  createEffect(() => {
    const bar = props.last;
    if (!bar || !candle) return;
    upsertLast(bar);
  });

  createEffect(() => {
    const colors = themeColors(props.theme);
    if (!chart) return;
    chart.applyOptions({
      layout: {
        background: { type: ColorType.Solid, color: colors.background },
        textColor: colors.text,
      },
      grid: {
        vertLines: { color: colors.grid },
        horzLines: { color: colors.grid },
      },
      crosshair: {
        mode: CrosshairMode.Normal,
        vertLine: { color: colors.crosshair, labelBackgroundColor: colors.crosshairLabel },
        horzLine: { color: colors.crosshair, labelBackgroundColor: colors.crosshairLabel },
      },
      rightPriceScale: { borderColor: colors.border },
      timeScale: { borderColor: colors.border },
    });
    if (volume) {
      volume.applyOptions({ color: colors.volume });
    }
  });

  function upsertLast(bar: Bar) {
    if (!candle || !volume) return;
    candle.update({
      time: bar.time as UTCTimestamp,
      open: bar.open,
      high: bar.high,
      low: bar.low,
      close: bar.close,
    });
    volume.update({
      time: bar.time as UTCTimestamp,
      value: bar.volume,
      color: bar.close >= bar.open ? "#16c78455" : "#ea394355",
    });

    if (!priceLine) {
      priceLine = candle.createPriceLine({
        price: bar.close,
        color: "#4f8cff",
        lineWidth: 1,
        lineStyle: 2,
        axisLabelVisible: true,
        title: "last",
      });
    } else {
      priceLine.applyOptions({ price: bar.close });
    }
  }

  return <div ref={container} class="chart-canvas" aria-label={`${props.symbol} K线图`} />;
}
