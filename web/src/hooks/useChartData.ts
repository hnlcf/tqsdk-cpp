import {
  createEffect,
  createMemo,
  createSignal,
  untrack,
  type Accessor,
} from "solid-js";
import {
  getKline,
  getQuote,
  type KlineAdjust,
  type KlineBar,
  type Quote,
} from "../api/client";
import type { OHLCV } from "../indicators/indicators";
import { mergeBars, toOHLCV } from "../chart/data";

interface Options {
  symbol: Accessor<string>;
  duration: Accessor<number>;
  adjust: Accessor<KlineAdjust>;
  onQuote: (symbol: string, quote: Quote) => void;
  onError: (message: string) => void;
}

/**
 * Owns chart history/live-bar merge and the request-generation guard used when
 * the selected symbol or period changes while a REST request is in flight.
 */
export function useChartData(options: Options) {
  const [historyVariants, setHistoryVariants] = createSignal<
    Record<KlineAdjust, OHLCV[]>
  >({ none: [], forward: [], back: [] });
  const [liveBars, setLiveBars] = createSignal<OHLCV[]>([]);
  let generation = 0;

  async function load(symbol: string, duration: number) {
    const current = ++generation;
    setHistoryVariants({ none: [], forward: [], back: [] });
    setLiveBars([]);
    try {
      const [quote, raw] = await Promise.all([
        getQuote(symbol),
        getKline(symbol, duration),
      ]);
      const variants: Record<KlineAdjust, OHLCV[]> = {
        none: toOHLCV(raw.bars),
        forward: [],
        back: [],
      };
      // 主连 1d 才有换月复权；一次性取回原始/前复权/后复权，切换时不发请求。
      if (symbol.startsWith("KQ.m@") && duration === 86_400_000_000_000) {
        const [forward, back] = await Promise.all([
          getKline(symbol, duration, "forward"),
          getKline(symbol, duration, "back"),
        ]);
        variants.forward = forward.adjusted ? toOHLCV(forward.bars) : variants.none;
        variants.back = back.adjusted ? toOHLCV(back.bars) : variants.none;
      }
      if (current !== generation) return;
      options.onQuote(symbol, quote);
      setHistoryVariants(variants);
    } catch (error) {
      if (current !== generation) return;
      options.onError(error instanceof Error ? error.message : String(error));
    }
  }

  function applyKline(symbol: string, duration: string, payload: unknown) {
    if (options.adjust() !== "none") return;
    if (symbol !== options.symbol() || Number(duration) !== options.duration()) return;
    const update = payload as { reset?: boolean; bars?: KlineBar[] };
    const incoming = toOHLCV(update.bars ?? []);
    if (incoming.length === 0) return;
    if (update.reset) {
      setLiveBars(incoming);
    } else {
      setLiveBars((previous) => mergeBars(previous, incoming));
    }
  }

  createEffect(() => {
    const symbol = options.symbol();
    const duration = options.duration();
    untrack(() => {
      void load(symbol, duration);
    });
  });

  const chartBars = createMemo(() => {
    const history = historyVariants()[options.adjust()] ?? historyVariants().none;
    if (options.adjust() !== "none") return history;
    return liveBars().length > 0 ? mergeBars(history, liveBars()) : history;
  });

  return { chartBars, applyKline };
}
