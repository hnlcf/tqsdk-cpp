import { createEffect, createMemo, createSignal, onCleanup, onMount } from "solid-js";
import {
  CandlestickSeries,
  ColorType,
  CrosshairMode,
  HistogramSeries,
  LineSeries,
  createChart,
  type CandlestickData,
  type IChartApi,
  type IPriceLine,
  type ISeriesApi,
  type SeriesType,
  type UTCTimestamp,
} from "lightweight-charts";
import {
  boll,
  macd,
  rsi,
  sma,
  type IndicatorConfig,
  type OHLCV,
} from "../indicators/indicators";
import { chartColors, type Theme } from "./theme";

interface Props {
  bars: OHLCV[];
  last?: OHLCV;
  symbol: string;
  theme: Theme;
  priceDecimals?: number;
  indicators?: IndicatorConfig;
}

type AnySeries = ISeriesApi<SeriesType>;

function toCandlestick(bar: OHLCV): CandlestickData<UTCTimestamp> {
  return {
    time: bar.time as UTCTimestamp,
    open: bar.open,
    high: bar.high,
    low: bar.low,
    close: bar.close,
  };
}

function formatLegendPrice(value: number | undefined, decimals: number): string {
  return typeof value === "number" && Number.isFinite(value) ? value.toFixed(decimals) : "—";
}

export default function KLineChart(props: Props) {
  let container: HTMLDivElement | undefined;
  let chart: IChartApi | undefined;
  let candle: ISeriesApi<"Candlestick"> | undefined;
  let volume: ISeriesApi<"Histogram"> | undefined;
  let priceLine: IPriceLine | undefined;
  let dataKey = "";
  const indicatorSeries = new Map<string, AnySeries>();
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

  function rebuildIndicators() {
    if (!chart || !candle) return;
    clearIndicators();
    const config = props.indicators;
    if (!config) {
      updateIndicators(props.bars);
      return;
    }
    const theme = colors();
    for (let index = 0; index < config.maPeriods.length; index++) {
      const period = config.maPeriods[index];
      addLine(`ma-${period}`, {
        color: config.maColors?.[index] ?? theme.ma[index % theme.ma.length],
        lineWidth: 1,
        priceLineVisible: false,
        lastValueVisible: false,
        crosshairMarkerVisible: false,
        title: `MA${period}`,
      });
    }
    if (config.boll) {
      const pane = 0;
      addLine(
        "boll-upper",
        {
          color: config.bollColors?.bollUpper ?? theme.bollUpper,
          lineWidth: 1,
          lineStyle: 2,
          priceLineVisible: false,
          lastValueVisible: false,
          crosshairMarkerVisible: false,
          title: "BOLL UP",
        },
        pane,
      );
      addLine(
        "boll-middle",
        {
          color: config.bollColors?.bollMiddle ?? theme.bollMiddle,
          lineWidth: 1,
          priceLineVisible: false,
          lastValueVisible: false,
          crosshairMarkerVisible: false,
          title: "BOLL MID",
        },
        pane,
      );
      addLine(
        "boll-lower",
        {
          color: config.bollColors?.bollLower ?? theme.bollLower,
          lineWidth: 1,
          lineStyle: 2,
          priceLineVisible: false,
          lastValueVisible: false,
          crosshairMarkerVisible: false,
          title: "BOLL LOW",
        },
        pane,
      );
    }
    if (config.macd) {
      ensurePane(1);
      const pane = 1;
      const histogram = chart.addSeries(
        HistogramSeries,
        {
          priceScaleId: "right",
          priceLineVisible: false,
          lastValueVisible: false,
          title: "MACD",
        },
        pane,
      );
      indicatorSeries.set("macd-histogram", histogram as AnySeries);
      addLine(
        "macd-dif",
        {
          color: config.macdColors?.macdDif ?? theme.macdDif,
          lineWidth: 1,
          priceLineVisible: false,
          lastValueVisible: false,
          crosshairMarkerVisible: false,
          title: "DIF",
        },
        pane,
      );
      addLine(
        "macd-dea",
        {
          color: config.macdColors?.macdDea ?? theme.macdDea,
          lineWidth: 1,
          priceLineVisible: false,
          lastValueVisible: false,
          crosshairMarkerVisible: false,
          title: "DEA",
        },
        pane,
      );
    }
    if (config.rsi) {
      const pane = config.macd ? 2 : 1;
      ensurePane(pane);
      addLine(
        "rsi",
        {
          color: config.rsiColors?.rsi ?? theme.average,
          lineWidth: 1,
          priceLineVisible: false,
          lastValueVisible: false,
          crosshairMarkerVisible: false,
          title: `RSI${config.rsiPeriod}`,
        },
        pane,
      );
    }
    updateIndicators(props.bars);
  }

  function updateIndicators(bars: OHLCV[]) {
    if (!chart || !candle) return;
    const config = props.indicators;
    if (!config) return;
    const theme = colors();
    for (const period of config.maPeriods) {
      indicatorSeries.get(`ma-${period}`)?.setData(
        sma(bars, period).map((point) => ({ time: point.time as UTCTimestamp, value: point.value })),
      );
    }
    if (config.boll) {
      const bands = boll(bars, config.bollPeriod, config.bollMultiplier);
      indicatorSeries.get("boll-upper")?.setData(
        bands.upper.map((point) => ({ time: point.time as UTCTimestamp, value: point.value })),
      );
      indicatorSeries.get("boll-middle")?.setData(
        bands.middle.map((point) => ({ time: point.time as UTCTimestamp, value: point.value })),
      );
      indicatorSeries.get("boll-lower")?.setData(
        bands.lower.map((point) => ({ time: point.time as UTCTimestamp, value: point.value })),
      );
    }
    if (config.macd) {
      const values = macd(bars, config.macdFast, config.macdSlow, config.macdSignal);
      const histogram = indicatorSeries.get("macd-histogram");
      const dif = indicatorSeries.get("macd-dif");
      const dea = indicatorSeries.get("macd-dea");
      histogram?.setData(
        values.histogram.map((point) => ({
          time: point.time as UTCTimestamp,
          value: point.value,
          color:
            point.value >= 0
              ? config.macdColors?.macdHistUp ?? theme.positive + "80"
              : config.macdColors?.macdHistDown ?? theme.negative + "80",
        })),
      );
      dif?.setData(
        values.dif.map((point) => ({ time: point.time as UTCTimestamp, value: point.value })),
      );
      dea?.setData(
        values.dea.map((point) => ({ time: point.time as UTCTimestamp, value: point.value })),
      );
    }
    if (config.rsi) {
      const values = rsi(bars, config.rsiPeriod);
      indicatorSeries.get("rsi")?.setData(
        values.map((point) => ({ time: point.time as UTCTimestamp, value: point.value })),
      );
    }
  }

  function updateSeriesData(forceFit: boolean) {
    if (!chart || !candle || !volume) return;
    const bars = props.bars;
    if (bars.length === 0) {
      candle.setData([]);
      volume.setData([]);
      if (priceLine) {
        candle.removePriceLine(priceLine);
        priceLine = undefined;
      }
      updateIndicators([]);
      return;
    }
    const nextKey = `${props.symbol}|${bars.length}|${bars[0].time}`;
    if (nextKey !== dataKey) {
      dataKey = nextKey;
      candle.setData(bars.map(toCandlestick));
      volume.setData(
        bars.map((bar) => ({
          time: bar.time as UTCTimestamp,
          value: bar.volume,
          color: bar.close >= bar.open ? colors().volumeUp : colors().volumeDown,
        })),
      );
      if (forceFit) chart.timeScale().fitContent();
    } else {
      const last = props.last ?? bars[bars.length - 1];
      candle.update(toCandlestick(last));
      volume.update({
        time: last.time as UTCTimestamp,
        value: last.volume,
        color: last.close >= last.open ? colors().volumeUp : colors().volumeDown,
      });
    }
    updateIndicators(bars);
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
      },
      localization: {
        locale: "zh-CN",
        priceFormatter: (price: number) => price.toFixed(decimals()),
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
      timeScale: { borderColor: theme.border },
      localization: { priceFormatter: (price: number) => price.toFixed(decimals()) },
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
    // Depend on symbol, data shape, config and theme so indicator series are
    // rebuilt only when their definition changes.
    props.symbol;
    JSON.stringify(props.indicators ?? {});
    colors();
    rebuildIndicators();
  });

  createEffect(() => {
    if (!ready()) return;
    props.bars;
    props.last;
    updateSeriesData(true);
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
        <span class="chart-credit">TradingView Lightweight Charts</span>
      </div>
      <div ref={container} class="chart-canvas" aria-label={`${props.symbol} K线图`} />
    </div>
  );
}
