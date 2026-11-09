import { For, Show, createEffect, createMemo, createSignal } from "solid-js";
import type {
  AccountSnapshot,
  OrderSnapshot,
  PositionSnapshot,
  Quote,
  TradeSnapshot,
} from "../api/client";
import { finite, formatMoney, formatPrice, formatVolume, priceDecimals } from "../lib/format";
import { fallbackVolumeMultiple, instrumentName } from "../lib/instruments";

type Tab = "positions" | "orders" | "trades";

interface Props {
  account?: AccountSnapshot;
  positions: Record<string, PositionSnapshot>;
  quotes: Record<string, Quote>;
  orders: Record<string, OrderSnapshot>;
  trades: Record<string, TradeSnapshot>;
  ready: boolean;
  onCancel: (orderId: string) => Promise<void>;
}

export interface PositionRow {
  symbol: string;
  name: string;
  direction: string;
  total: number;
  openAverage: number | null;
  lastPrice: number | null;
  margin: number;
  profit: number;
  profitRatio: number | null;
  marketValue: number | null;
  capitalRatio: number | null;
  decimals: number;
}

const DIRECTION_LABEL: Record<string, string> = {
  BUY: "买",
  SELL: "卖",
};

const OFFSET_LABEL: Record<string, string> = {
  OPEN: "开",
  CLOSE: "平",
  CLOSETODAY: "平今",
  CLOSEYESTERDAY: "平昨",
};

function tradeTime(value?: number): string {
  if (!value) return "—";
  const milliseconds = value > 1e15 ? Math.floor(value / 1e6) : value;
  return new Date(milliseconds).toLocaleTimeString("zh-CN", { hour12: false });
}

function ratioText(value: number | null): string {
  return value === null || !Number.isFinite(value) ? "—" : `${(value * 100).toFixed(2)}%`;
}

function weightedOpen(
  longVolume: number,
  shortVolume: number,
  longPrice: number | null | undefined,
  shortPrice: number | null | undefined,
): number | null {
  const parts: Array<{ volume: number; price: number }> = [];
  if (longVolume > 0 && finite(longPrice)) parts.push({ volume: longVolume, price: longPrice });
  if (shortVolume > 0 && finite(shortPrice)) parts.push({ volume: shortVolume, price: shortPrice });
  if (parts.length === 0) return null;
  const volume = parts.reduce((sum, part) => sum + part.volume, 0);
  return parts.reduce((sum, part) => sum + part.price * part.volume, 0) / volume;
}

/**
 * One row per contract: long/short are merged into total volume and one
 * weighted open price. Latest price and P&L are recomputed from live quotes
 * so the table does not wait for a new CTP position query.
 */
export function buildPositionRows(
  positions: Record<string, PositionSnapshot>,
  quotes: Record<string, Quote>,
  account: AccountSnapshot | undefined,
): PositionRow[] {
  return Object.values(positions)
    .map((row) => {
      const symbol = row.instrument_id;
      const quote = quotes[symbol];
      const long = row.volume_long ?? 0;
      const short = row.volume_short ?? 0;
      const total = long + short;
      if (total <= 0) return null;

      const decimals = finite(row.price_decs)
        ? Math.max(0, Math.min(6, row.price_decs))
        : priceDecimals(quote, 2);
      const lastPrice = finite(quote?.last_price)
        ? quote.last_price
        : finite(row.last_price)
          ? row.last_price
          : null;
      const longOpen = row.open_price_long ?? row.position_price_long;
      const shortOpen = row.open_price_short ?? row.position_price_short;
      const openAverage = weightedOpen(long, short, longOpen, shortOpen);
      const multiplier =
        (finite(row.volume_multiple) && row.volume_multiple > 0
          ? row.volume_multiple
          : finite(quote?.volume_multiple) && quote.volume_multiple > 0
            ? quote.volume_multiple
            : fallbackVolumeMultiple(symbol)) || 1;

      const canRecompute =
        lastPrice !== null &&
        (long === 0 || finite(longOpen)) &&
        (short === 0 || finite(shortOpen));
      const longProfit = finite(longOpen) ? (lastPrice! - longOpen) * long * multiplier : 0;
      const shortProfit = finite(shortOpen) ? (shortOpen - lastPrice!) * short * multiplier : 0;
      const profit = canRecompute
        ? longProfit + shortProfit
        : (row.float_profit_long ?? 0) + (row.float_profit_short ?? 0) || row.float_profit || 0;
      const margin = row.margin ?? (row.margin_long ?? 0) + (row.margin_short ?? 0);
      const marketValue = lastPrice === null ? null : total * lastPrice * multiplier;
      const capitalRatio =
        account?.balance && account.balance > 0 ? margin / account.balance : null;
      const profitRatio = margin > 0 ? profit / margin : null;

      return {
        symbol,
        name: row.instrument_name || quote?.instrument_name || instrumentName(symbol),
        direction: long > 0 && short > 0 ? "双向" : long > 0 ? "多" : "空",
        total,
        openAverage,
        lastPrice,
        margin,
        profit,
        profitRatio,
        marketValue,
        capitalRatio,
        decimals,
      };
    })
    .filter((row): row is PositionRow => row !== null)
    .sort((a, b) => a.symbol.localeCompare(b.symbol));
}

export default function TradingTables(props: Props) {
  const [tab, setTab] = createSignal<Tab>("positions");
  const [quoteRevision, setQuoteRevision] = createSignal(0);

  // Keep the merged table tied to quote changes explicitly. Without this
  // revision hook a long-lived For row can keep the price snapshot from the
  // previous position poll on some Solid runtimes.
  createEffect(() => {
    props.quotes;
    setQuoteRevision((value) => value + 1);
  });

  const positionRows = createMemo(() => {
    quoteRevision();
    return buildPositionRows(props.positions, props.quotes, props.account);
  });
  const orderRows = createMemo(() =>
    Object.values(props.orders).sort(
      (a, b) => (b.insert_date_time ?? 0) - (a.insert_date_time ?? 0),
    ),
  );
  const tradeRows = createMemo(() =>
    Object.values(props.trades).sort(
      (a, b) => (b.trade_date_time ?? 0) - (a.trade_date_time ?? 0),
    ),
  );
  const activeOrders = createMemo(() => orderRows().filter((row) => !row.is_dead).length);

  return (
    <section class="trading-section">
      <div class="account-cards">
        <div class="account-card">
          <small>账户权益</small>
          <strong>{formatMoney(props.account?.balance)}</strong>
        </div>
        <div class="account-card">
          <small>可用资金</small>
          <strong>{formatMoney(props.account?.available)}</strong>
        </div>
        <div class="account-card">
          <small>占用保证金</small>
          <strong>{formatMoney(props.account?.margin)}</strong>
        </div>
        <div
          class={`account-card ${(props.account?.float_profit ?? 0) >= 0 ? "positive" : "negative"}`}
        >
          <small>浮动盈亏</small>
          <strong>{formatMoney(props.account?.float_profit)}</strong>
        </div>
        <div class="account-card">
          <small>风险度</small>
          <strong>
            {typeof props.account?.risk_ratio === "number"
              ? `${(props.account.risk_ratio * 100).toFixed(2)}%`
              : "—"}
          </strong>
        </div>
      </div>

      <div class="panel table-panel trading-tables">
        <div class="table-tabs">
          <button
            type="button"
            class={tab() === "positions" ? "active" : ""}
            onClick={() => setTab("positions")}
          >
            持仓 <span>{positionRows().length}</span>
          </button>
          <button
            type="button"
            class={tab() === "orders" ? "active" : ""}
            onClick={() => setTab("orders")}
          >
            委托 <span>{activeOrders()}</span>
          </button>
          <button
            type="button"
            class={tab() === "trades" ? "active" : ""}
            onClick={() => setTab("trades")}
          >
            成交 <span>{tradeRows().length}</span>
          </button>
          <span class={`source-badge ${props.ready ? "live" : ""}`}>
            {props.ready ? "TRADING" : "READONLY"}
          </span>
        </div>

        <Show when={tab() === "positions"}>
          <div class="table-scroll">
            <table>
              <thead>
                <tr>
                  <th>合约</th>
                  <th>合约名</th>
                  <th>多空</th>
                  <th>持仓数</th>
                  <th>开仓均价</th>
                  <th>最新价</th>
                  <th>保证金</th>
                  <th>浮动盈亏</th>
                  <th title="浮动盈亏 / 占用保证金">浮盈比例</th>
                  <th>持仓市值</th>
                  <th title="保证金 / 账户权益">资金占比</th>
                </tr>
              </thead>
              <tbody>
                <For each={positionRows()}>
                  {(row) => (
                    <tr>
                      <td class="mono strong">{row.symbol}</td>
                      <td>{row.name}</td>
                      <td
                        class={
                          row.direction === "多"
                            ? "positive"
                            : row.direction === "空"
                              ? "negative"
                              : ""
                        }
                      >
                        {row.direction}
                      </td>
                      <td class="mono strong">{formatVolume(row.total)}</td>
                      <td class="mono">{formatPrice(row.openAverage, row.decimals)}</td>
                      <td class="mono">{formatPrice(row.lastPrice, row.decimals)}</td>
                      <td class="mono">{formatMoney(row.margin)}</td>
                      <td class={row.profit >= 0 ? "positive" : "negative"}>
                        {formatMoney(row.profit)}
                      </td>
                      <td class={row.profit >= 0 ? "positive" : "negative"}>
                        {ratioText(row.profitRatio)}
                      </td>
                      <td class="mono">{formatMoney(row.marketValue)}</td>
                      <td class="mono">{ratioText(row.capitalRatio)}</td>
                    </tr>
                  )}
                </For>
              </tbody>
            </table>
            <Show when={positionRows().length === 0}>
              <div class="empty-state">当前没有持仓</div>
            </Show>
          </div>
        </Show>

        <Show when={tab() === "orders"}>
          <div class="table-scroll">
            <table>
              <thead>
                <tr>
                  <th>订单号</th>
                  <th>合约</th>
                  <th>方向</th>
                  <th>开平</th>
                  <th>价格</th>
                  <th>手数</th>
                  <th>剩余</th>
                  <th>状态</th>
                  <th>操作</th>
                </tr>
              </thead>
              <tbody>
                <For each={orderRows()}>
                  {(row) => (
                    <tr>
                      <td class="mono">{row.order_id}</td>
                      <td>{row.instrument_id}</td>
                      <td class={row.direction === "BUY" ? "positive" : "negative"}>
                        {DIRECTION_LABEL[row.direction ?? ""] ?? row.direction ?? "—"}
                      </td>
                      <td>{OFFSET_LABEL[row.offset ?? ""] ?? row.offset ?? "—"}</td>
                      <td class="mono">{formatPrice(row.limit_price, 2)}</td>
                      <td class="mono">{formatVolume(row.volume_orign)}</td>
                      <td class="mono">{formatVolume(row.volume_left)}</td>
                      <td>
                        <span class={`order-status ${row.is_dead ? "done" : "alive"}`}>
                          {row.last_msg || row.status || (row.is_dead ? "FINISHED" : "ALIVE")}
                        </span>
                      </td>
                      <td>
                        <Show when={!row.is_dead}>
                          <button
                            type="button"
                            class="table-action"
                            onClick={() => void props.onCancel(row.order_id)}
                          >
                            撤单
                          </button>
                        </Show>
                      </td>
                    </tr>
                  )}
                </For>
              </tbody>
            </table>
            <Show when={orderRows().length === 0}>
              <div class="empty-state">当前没有委托</div>
            </Show>
          </div>
        </Show>

        <Show when={tab() === "trades"}>
          <div class="table-scroll">
            <table>
              <thead>
                <tr>
                  <th>成交号</th>
                  <th>合约</th>
                  <th>方向</th>
                  <th>开平</th>
                  <th>价格</th>
                  <th>手数</th>
                  <th>时间</th>
                </tr>
              </thead>
              <tbody>
                <For each={tradeRows()}>
                  {(row) => (
                    <tr>
                      <td class="mono">{row.trade_id}</td>
                      <td>{row.instrument_id}</td>
                      <td class={row.direction === "BUY" ? "positive" : "negative"}>
                        {DIRECTION_LABEL[row.direction ?? ""] ?? row.direction ?? "—"}
                      </td>
                      <td>{OFFSET_LABEL[row.offset ?? ""] ?? row.offset ?? "—"}</td>
                      <td class="mono">{formatPrice(row.price, 2)}</td>
                      <td class="mono">{formatVolume(row.volume)}</td>
                      <td class="mono">{tradeTime(row.trade_date_time)}</td>
                    </tr>
                  )}
                </For>
              </tbody>
            </table>
            <Show when={tradeRows().length === 0}>
              <div class="empty-state">当前没有成交</div>
            </Show>
          </div>
        </Show>
      </div>
    </section>
  );
}
