import {
  createEffect,
  createMemo,
  createSignal,
  onCleanup,
  onMount,
  untrack,
  type Accessor,
} from "solid-js";
import {
  cancelOrder as cancelOrderApi,
  getAccounts,
  getReadyz,
  getTradingSnapshot,
  insertOrder,
  type AccountSnapshot,
  type AccountsResponse,
  type OrderSnapshot,
  type PositionSnapshot,
  type ReadyState,
  type TradeData,
  type TradeSnapshot,
} from "../api/client";
import type { TicketOrder } from "../components/OrderTicketPanel";
import { mergeTradeData } from "../lib/tradingData";

type StreamStatus = "connecting" | "online" | "offline";

interface Options {
  account: Accessor<string>;
  setAccount: (account: string) => void;
  symbol: Accessor<string>;
  streamStatus: Accessor<StreamStatus>;
  notify: (message: string) => void;
}

/**
 * Owns REST trading snapshots, account discovery/polling and order actions.
 * WebSocket diffs are fed through applyTradeData so REST recovery and live data
 * share the same merge semantics.
 */
export function useTradingSession(options: Options) {
  const [ready, setReady] = createSignal<ReadyState>();
  const [accounts, setAccounts] = createSignal<AccountsResponse>();
  const [account, setAccount] = createSignal<AccountSnapshot>();
  const [positions, setPositions] = createSignal<Record<string, PositionSnapshot>>({});
  const [orders, setOrders] = createSignal<Record<string, OrderSnapshot>>({});
  const [trades, setTrades] = createSignal<Record<string, TradeSnapshot>>({});
  let snapshotLoaded = false;
  let refreshInFlight = false;

  const defaultAccountId = createMemo(() => accounts()?.default ?? "default");
  const isDefaultAccount = createMemo(() => options.account() === defaultAccountId());
  const selectedAccountInfo = createMemo(() =>
    accounts()?.accounts.find((item) => item.account_id === options.account()),
  );
  const tradingReady = createMemo(
    () =>
      Boolean(ready()?.ready) &&
      options.streamStatus() === "online" &&
      (isDefaultAccount() || Boolean(selectedAccountInfo()?.ready)),
  );

  function applyTradeData(data: TradeData, snapshot: boolean) {
    const next = mergeTradeData(
      { account: account(), positions: positions(), orders: orders(), trades: trades() },
      data,
      snapshot,
    );
    setAccount(next.account);
    setPositions(next.positions);
    setOrders(next.orders);
    setTrades(next.trades);
  }

  async function refreshReady() {
    try {
      setReady(await getReadyz());
    } catch {
      setReady(undefined);
    }
  }

  async function loadAccounts() {
    try {
      const data = await getAccounts();
      setAccounts(data);
      const ids = data.accounts.map((item) => item.account_id);
      if (!ids.includes(options.account())) {
        options.setAccount(data.default || ids[0] || "default");
      }
    } catch {
      // Keep the last known account list; health polling will retry.
    }
  }

  async function loadSnapshot(accountId = options.account()) {
    try {
      applyTradeData(await getTradingSnapshot(undefined, accountId), true);
      snapshotLoaded = true;
    } catch {
      snapshotLoaded = false;
    }
  }

  async function refreshSnapshot() {
    if (refreshInFlight || !tradingReady()) return;
    refreshInFlight = true;
    try {
      applyTradeData(await getTradingSnapshot(undefined, options.account()), true);
      snapshotLoaded = true;
    } catch {
      // Keep the last WS snapshot; the next poll/reconnect will resync.
    } finally {
      refreshInFlight = false;
    }
  }

  async function submitTicket(order: TicketOrder): Promise<string> {
    if (!tradingReady()) throw new Error("交易网关未就绪");
    if (order.priceType === "LIMIT" && (!Number.isFinite(order.price) || order.price <= 0)) {
      throw new Error("请输入有效限价");
    }
    const symbol = options.symbol();
    const result = await insertOrder(
      {
        instrument_id: symbol,
        exchange_id: symbol.split(".")[0],
        direction: order.side,
        offset: order.offset,
        price_type: order.priceType,
        limit_price: order.priceType === "LIMIT" ? order.price : 0,
        time_cond: order.priceType === "LIMIT" && order.advanced === "NONE" ? "GFD" : "IOC",
        volume_cond: order.advanced === "FOK" ? "ALL" : order.volumeCond,
        min_volume: order.volumeCond === "MIN" ? order.minVolume : 0,
        advanced: order.advanced,
        volume: order.volume,
      },
      options.account(),
    );
    if (!result.success) throw new Error(result.error || "报单失败");
    void refreshReady();
    return result.order_id ? `已提交 ${result.order_id}` : "已提交";
  }

  async function cancelLiveOrder(orderId: string) {
    try {
      const result = await cancelOrderApi(orderId, options.account());
      options.notify(result.success ? `撤单已提交 ${orderId}` : result.error || "撤单失败");
    } catch (error) {
      options.notify(error instanceof Error ? error.message : String(error));
    }
  }

  createEffect(() => {
    const accountId = options.account();
    untrack(() => {
      snapshotLoaded = false;
      setAccount(undefined);
      setPositions({});
      setOrders({});
      setTrades({});
      void loadSnapshot(accountId);
    });
  });

  onMount(() => {
    void refreshReady();
    void loadAccounts();
    void loadSnapshot();
    const healthTimer = window.setInterval(() => {
      void refreshReady();
      void loadAccounts();
      if (!snapshotLoaded && ready()?.ready) void loadSnapshot();
    }, 3000);
    const tradingTimer = window.setInterval(() => {
      void refreshSnapshot();
    }, 5000);
    onCleanup(() => {
      window.clearInterval(healthTimer);
      window.clearInterval(tradingTimer);
    });
  });

  return {
    ready,
    accounts,
    account,
    positions,
    orders,
    trades,
    defaultAccountId,
    isDefaultAccount,
    selectedAccountInfo,
    tradingReady,
    applyTradeData,
    submitTicket,
    cancelLiveOrder,
  };
}
