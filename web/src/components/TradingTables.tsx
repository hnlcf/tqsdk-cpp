import { For, Show, createMemo, createSignal } from "solid-js";
import type {
  AccountSnapshot,
  OrderSnapshot,
  PositionSnapshot,
  TradeSnapshot,
} from "../api/client";
import { formatMoney, formatPrice, formatVolume, priceDecimals } from "../lib/format";

type Tab = "positions" | "orders" | "trades";

interface Props {
  account?: AccountSnapshot;
  positions: Record<string, PositionSnapshot>;
  orders: Record<string, OrderSnapshot>;
  trades: Record<string, TradeSnapshot>;
  ready: boolean;
  onCancel: (orderId: string) => Promise<void>;
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

export default function TradingTables(props: Props) {
  const [tab, setTab] = createSignal<Tab>("positions");

  const positionRows = createMemo(() =>
    Object.values(props.positions)
      .filter((row) => (row.volume_long ?? 0) > 0 || (row.volume_short ?? 0) > 0)
      .sort((a, b) => a.instrument_id.localeCompare(b.instrument_id)),
  );
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
  const decimalsFor = (symbol: string) => priceDecimals(props.positions[symbol] as never, 2);

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
                  <th>多手</th>
                  <th>空手</th>
                  <th>多均价</th>
                  <th>空均价</th>
                  <th>最新</th>
                  <th>保证金</th>
                  <th>浮动盈亏</th>
                </tr>
              </thead>
              <tbody>
                <For each={positionRows()}>
                  {(row) => {
                    const profit =
                      (row.float_profit_long ?? 0) + (row.float_profit_short ?? 0) ||
                      row.float_profit ||
                      0;
                    return (
                      <tr>
                        <td class="mono strong">{row.instrument_id}</td>
                        <td class="positive">{formatVolume(row.volume_long)}</td>
                        <td class="negative">{formatVolume(row.volume_short)}</td>
                        <td class="mono">{formatPrice(row.open_price_long, decimalsFor(row.instrument_id))}</td>
                        <td class="mono">{formatPrice(row.open_price_short, decimalsFor(row.instrument_id))}</td>
                        <td class="mono">{formatPrice(row.last_price, decimalsFor(row.instrument_id))}</td>
                        <td class="mono">{formatMoney(row.margin)}</td>
                        <td class={profit >= 0 ? "positive" : "negative"}>{formatMoney(profit)}</td>
                      </tr>
                    );
                  }}
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
