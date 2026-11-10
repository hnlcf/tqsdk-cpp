import { createEffect, onCleanup, untrack, type Accessor } from "solid-js";
import {
  createMarketStream,
  type KlineSubscription,
  type MarketHandlers,
  type MarketStream,
  type TradeData,
} from "../api/client";

type StreamStatus = "connecting" | "online" | "offline";

interface Options {
  /** True after initial App setup; false disables connection ownership. */
  enabled: Accessor<boolean>;
  /** Selected account id; switching closes the old stream and opens a new one. */
  account: Accessor<string>;
  onStatus?: (status: StreamStatus, account: string) => void;
  onQuote?: MarketHandlers["onQuote"];
  onTradeData?: (data: TradeData, snapshot: boolean, account: string) => void;
  onKline?: MarketHandlers["onKline"];
}

/**
 * Owns the account-scoped WebSocket lifecycle and makes subscription updates
 * independent from App's render/controller code.
 */
export function useMarketStream(options: Options) {
  let stream: MarketStream | undefined;

  const connect = (account: string) =>
    createMarketStream(
      {
        onStatus: (status) => options.onStatus?.(status, account),
        onQuote: (quotes) => options.onQuote?.(quotes),
        onTradeData: (data, snapshot) => options.onTradeData?.(data, snapshot, account),
        onKline: (symbol, duration, payload) =>
          options.onKline?.(symbol, duration, payload),
      },
      account === "default" ? undefined : account,
    );

  createEffect(() => {
    if (!options.enabled()) {
      untrack(() => {
        stream?.close();
        stream = undefined;
      });
      return;
    }
    const account = options.account();
    untrack(() => {
      stream?.close();
      stream = connect(account);
    });
  });

  onCleanup(() => {
    stream?.close();
    stream = undefined;
  });

  return {
    setSubscription(
      symbols: string[],
      klines: KlineSubscription[],
      topics: string[],
    ) {
      stream?.setSubscription(symbols, klines, topics);
    },
  };
}
