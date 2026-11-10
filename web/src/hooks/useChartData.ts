import {
  createEffect,
  createMemo,
  createSignal,
  untrack,
  type Accessor,
} from "solid-js";
import { getKline, getQuote, type KlineBar, type Quote } from "../api/client";
import type { OHLCV } from "../indicators/indicators";
import { mergeBars, toOHLCV } from "../chart/data";

interface Options {
  symbol: Accessor<string>;
  duration: Accessor<number>;
  onQuote: (symbol: string, quote: Quote) => void;
  onError: (message: string) => void;
}

/**
 * Owns chart history/live-bar merge and the request-generation guard used when
 * the selected symbol or period changes while a REST request is in flight.
 */
export function useChartData(options: Options) {
  const [history, setHistory] = createSignal<OHLCV[]>([]);
  const [liveBars, setLiveBars] = createSignal<OHLCV[]>([]);
  let generation = 0;

  async function load(symbol: string, duration: number) {
    const current = ++generation;
    setHistory([]);
    setLiveBars([]);
    try {
      const [quote, kline] = await Promise.all([
        getQuote(symbol),
        getKline(symbol, duration),
      ]);
      if (current !== generation) return;
      options.onQuote(symbol, quote);
      setHistory(toOHLCV(kline.bars));
    } catch (error) {
      if (current !== generation) return;
      options.onError(error instanceof Error ? error.message : String(error));
    }
  }

  function applyKline(symbol: string, duration: string, payload: unknown) {
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

  const chartBars = createMemo(() =>
    liveBars().length > 0 ? mergeBars(history(), liveBars()) : history(),
  );

  return { chartBars, applyKline };
}
