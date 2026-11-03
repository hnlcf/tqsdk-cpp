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
}

export default function KLineChart(props: Props) {
  let container: HTMLDivElement | undefined;
  let chart: IChartApi | undefined;
  let candle: ISeriesApi<"Candlestick"> | undefined;
  let volume: ISeriesApi<"Histogram"> | undefined;
  let priceLine: IPriceLine | undefined;

  onMount(() => {
    if (!container) return;

    chart = createChart(container, {
      autoSize: true,
      layout: {
        background: { type: ColorType.Solid, color: "#0b1016" },
        textColor: "#9aa7b8",
        fontFamily: "Inter, SFMono-Regular, Menlo, monospace",
        attributionLogo: true,
      },
      grid: {
        vertLines: { color: "#151f2b" },
        horzLines: { color: "#151f2b" },
      },
      crosshair: {
        mode: CrosshairMode.Normal,
        vertLine: { color: "#52657a", labelBackgroundColor: "#223043" },
        horzLine: { color: "#52657a", labelBackgroundColor: "#223043" },
      },
      rightPriceScale: {
        borderColor: "#223043",
        scaleMargins: { top: 0.08, bottom: 0.28 },
      },
      timeScale: {
        borderColor: "#223043",
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
      color: "#38506a",
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
    const bar = props.last;
    if (!bar || !candle) return;
    upsertLast(bar);
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
