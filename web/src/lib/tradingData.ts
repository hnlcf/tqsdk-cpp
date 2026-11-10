import type {
  AccountSnapshot,
  OrderSnapshot,
  PositionSnapshot,
  TradeData,
  TradeSnapshot,
} from "../api/client";

export interface TradingState {
  account?: AccountSnapshot;
  positions: Record<string, PositionSnapshot>;
  orders: Record<string, OrderSnapshot>;
  trades: Record<string, TradeSnapshot>;
}

/** Snapshot replaces; diff merges by id/symbol. */
export function mergeTradeData(
  previous: TradingState,
  incoming: TradeData,
  snapshot: boolean,
): TradingState {
  const account = incoming.account ?? previous.account;
  if (snapshot) {
    return {
      account,
      positions: incoming.positions ?? {},
      orders: incoming.orders ?? {},
      trades: incoming.trades ?? {},
    };
  }
  return {
    account,
    positions: incoming.positions
      ? { ...previous.positions, ...incoming.positions }
      : previous.positions,
    orders: incoming.orders ? { ...previous.orders, ...incoming.orders } : previous.orders,
    trades: incoming.trades ? { ...previous.trades, ...incoming.trades } : previous.trades,
  };
}
