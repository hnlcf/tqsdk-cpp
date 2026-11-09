import { For, Show, createSignal } from "solid-js";
import type { AccountSnapshot, Quote } from "../api/client";
import { formatMoney, formatPrice } from "../lib/format";

export type TicketSide = "BUY" | "SELL";
export type TicketOffset = "OPEN" | "CLOSE" | "CLOSETODAY" | "CLOSEYESTERDAY";
export type TicketPriceType = "LIMIT" | "BEST" | "ANY";

export interface TicketOrder {
  side: TicketSide;
  offset: TicketOffset;
  priceType: TicketPriceType;
  price: number;
  volume: number;
}

interface Props {
  symbol: string;
  quote?: Quote;
  account?: AccountSnapshot;
  ready: boolean;
  decimals: number;
  price: number;
  onPriceChange: (price: number) => void;
  onSubmit: (order: TicketOrder) => Promise<string>;
}

const PRICE_TYPES: Array<{ value: TicketPriceType; label: string; hint: string }> = [
  { value: "LIMIT", label: "限价", hint: "指定价格" },
  { value: "BEST", label: "对价", hint: "买一/卖一" },
  { value: "ANY", label: "市价", hint: "市价成交" },
];

const ACTIONS: Array<{
  side: TicketSide;
  offset: TicketOffset;
  label: string;
  hint: string;
  tone: "buy" | "sell" | "close";
}> = [
  { side: "BUY", offset: "OPEN", label: "买开", hint: "BUY OPEN", tone: "buy" },
  { side: "SELL", offset: "OPEN", label: "卖开", hint: "SELL OPEN", tone: "sell" },
  { side: "SELL", offset: "CLOSE", label: "平仓", hint: "SELL CLOSE", tone: "close" },
];

export default function OrderTicketPanel(props: Props) {
  const [priceType, setPriceType] = createSignal<TicketPriceType>("LIMIT");
  const [volume, setVolume] = createSignal(1);
  const [message, setMessage] = createSignal("设置价格与数量后，直接点三键下单");
  const [busy, setBusy] = createSignal(false);

  const tick = () => {
    const value = props.quote?.price_tick;
    return typeof value === "number" && value > 0 ? value : 1;
  };
  const isLimit = () => priceType() === "LIMIT";
  const priceHint = () =>
    priceType() === "BEST"
      ? `买一 ${formatPrice(props.quote?.bid_price1, props.decimals)} / 卖一 ${formatPrice(
          props.quote?.ask_price1,
          props.decimals,
        )}`
      : "以对手价/市价成交";

  function stepPrice(direction: -1 | 1) {
    const next = Math.max(tick(), props.price + direction * tick());
    props.onPriceChange(Number(next.toFixed(Math.max(0, props.decimals))));
  }

  async function submit(action: (typeof ACTIONS)[number]) {
    if (!props.ready) {
      setMessage("交易网关未就绪，禁止报单");
      return;
    }
    if (isLimit() && (!Number.isFinite(props.price) || props.price <= 0)) {
      setMessage("请输入有效限价");
      return;
    }
    const qty = Math.max(1, Math.floor(volume()));
    setBusy(true);
    setMessage(`提交${action.label}...`);
    try {
      const result = await props.onSubmit({
        side: action.side,
        offset: action.offset,
        priceType: priceType(),
        price: isLimit() ? props.price : 0,
        volume: qty,
      });
      setMessage(result);
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    } finally {
      setBusy(false);
    }
  }

  return (
    <aside class="panel ticket">
      <div class="panel-heading">
        <div>
          <span class="eyebrow">ORDER ENTRY</span>
          <h2>三键下单</h2>
        </div>
        <span class={`demo-pill ${props.ready ? "live" : ""}`}>
          {props.ready ? "READY" : "LOCKED"}
        </span>
      </div>

      <div class="order-summary">
        <span>合约</span>
        <strong>{props.symbol}</strong>
      </div>
      <div class="order-summary">
        <span>可用资金</span>
        <strong>{formatMoney(props.account?.available)}</strong>
      </div>

      <div class="order-form">
        <label class="field">
          <span>价格类型</span>
          <select
            value={priceType()}
            onChange={(event) => setPriceType(event.currentTarget.value as TicketPriceType)}
          >
            <For each={PRICE_TYPES}>
              {(item) => <option value={item.value}>{item.label} · {item.hint}</option>}
            </For>
          </select>
        </label>

        <label class="field">
          <span>价格</span>
          <Show
            when={isLimit()}
            fallback={<div class="market-price-hint">{priceHint()}</div>}
          >
            <div class="input-with-step">
              <button type="button" onClick={() => stepPrice(-1)}>
                −
              </button>
              <input
                type="number"
                min={tick()}
                step={tick()}
                value={Number.isFinite(props.price) ? props.price : ""}
                onInput={(event) => props.onPriceChange(Number(event.currentTarget.value))}
              />
              <button type="button" onClick={() => stepPrice(1)}>
                +
              </button>
            </div>
          </Show>
        </label>

        <label class="field">
          <span>数量</span>
          <div class="input-with-step">
            <button type="button" onClick={() => setVolume((value) => Math.max(1, value - 1))}>
              −
            </button>
            <input
              type="number"
              min="1"
              value={volume()}
              onInput={(event) => setVolume(Math.max(1, Number(event.currentTarget.value)))}
            />
            <button type="button" onClick={() => setVolume((value) => value + 1)}>
              +
            </button>
          </div>
        </label>
      </div>

      <div class="quick-orders three-key">
        <For each={ACTIONS}>
          {(action) => (
            <button
              type="button"
              class={`quick-order ${action.tone}`}
              disabled={!props.ready || busy()}
              onClick={() => void submit(action)}
            >
              <strong>{busy() ? "提交中" : action.label}</strong>
              <small>{action.hint}</small>
            </button>
          )}
        </For>
      </div>

      <p class="ticket-message">{message()}</p>
      <Show when={!props.ready}>
        <div class="ticket-lock">node /readyz 未就绪或 WebSocket 断开时禁止报单。</div>
      </Show>
    </aside>
  );
}
