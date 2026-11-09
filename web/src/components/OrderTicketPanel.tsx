import { For, Show, createSignal } from "solid-js";
import type { AccountSnapshot, Quote } from "../api/client";
import { formatMoney, formatPrice } from "../lib/format";

export type TicketSide = "BUY" | "SELL";
export type TicketOffset = "OPEN" | "CLOSE" | "CLOSETODAY" | "CLOSEYESTERDAY";

export interface TicketOrder {
  side: TicketSide;
  offset: TicketOffset;
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

const OFFSETS: Array<{ value: TicketOffset; label: string }> = [
  { value: "OPEN", label: "开仓" },
  { value: "CLOSE", label: "平仓" },
  { value: "CLOSETODAY", label: "平今" },
  { value: "CLOSEYESTERDAY", label: "平昨" },
];

export default function OrderTicketPanel(props: Props) {
  const [side, setSide] = createSignal<TicketSide>("BUY");
  const [offset, setOffset] = createSignal<TicketOffset>("OPEN");
  const [volume, setVolume] = createSignal(1);
  const [message, setMessage] = createSignal("行情就绪后可提交限价单");
  const [submitting, setSubmitting] = createSignal(false);

  const tick = () => {
    const value = props.quote?.price_tick;
    return typeof value === "number" && value > 0 ? value : 1;
  };

  function choose(nextSide: TicketSide, nextOffset: TicketOffset) {
    setSide(nextSide);
    setOffset(nextOffset);
  }

  function stepPrice(direction: -1 | 1) {
    const next = Math.max(tick(), props.price + direction * tick());
    props.onPriceChange(Number(next.toFixed(Math.max(0, props.decimals))));
  }

  async function submit(event: SubmitEvent) {
    event.preventDefault();
    if (!props.ready) {
      setMessage("交易网关未就绪，禁止报单");
      return;
    }
    if (!Number.isFinite(props.price) || props.price <= 0) {
      setMessage("请输入有效价格");
      return;
    }
    const qty = Math.max(1, Math.floor(volume()));
    setSubmitting(true);
    setMessage("提交中...");
    try {
      const result = await props.onSubmit({
        side: side(),
        offset: offset(),
        price: props.price,
        volume: qty,
      });
      setMessage(result);
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    } finally {
      setSubmitting(false);
    }
  }

  return (
    <aside class="panel ticket">
      <div class="panel-heading">
        <div>
          <span class="eyebrow">ORDER ENTRY</span>
          <h2>下单板</h2>
        </div>
        <span class={`demo-pill ${props.ready ? "live" : ""}`}>
          {props.ready ? "READY" : "LOCKED"}
        </span>
      </div>

      <div class="quick-orders">
        <button
          type="button"
          class={`quick-order buy ${side() === "BUY" && offset() === "OPEN" ? "active" : ""}`}
          onClick={() => choose("BUY", "OPEN")}
        >
          <strong>买开</strong>
          <small>BUY OPEN</small>
        </button>
        <button
          type="button"
          class={`quick-order sell ${side() === "SELL" && offset() === "OPEN" ? "active" : ""}`}
          onClick={() => choose("SELL", "OPEN")}
        >
          <strong>卖开</strong>
          <small>SELL OPEN</small>
        </button>
        <button
          type="button"
          class={`quick-order close ${side() === "SELL" && offset() !== "OPEN" ? "active" : ""}`}
          onClick={() => choose("SELL", "CLOSE")}
        >
          <strong>平仓</strong>
          <small>SELL CLOSE</small>
        </button>
      </div>

      <form onSubmit={submit}>
        <div class="side-switch">
          <button
            type="button"
            class={`buy ${side() === "BUY" ? "active" : ""}`}
            onClick={() => setSide("BUY")}
          >
            买入
          </button>
          <button
            type="button"
            class={`sell ${side() === "SELL" ? "active" : ""}`}
            onClick={() => setSide("SELL")}
          >
            卖出
          </button>
        </div>

        <label class="field">
          <span>开平</span>
          <select
            value={offset()}
            onChange={(event) => setOffset(event.currentTarget.value as TicketOffset)}
          >
            <For each={OFFSETS}>
              {(item) => <option value={item.value}>{item.label}</option>}
            </For>
          </select>
        </label>

        <label class="field">
          <span>价格</span>
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

        <div class="order-summary">
          <span>合约</span>
          <strong>{props.symbol}</strong>
        </div>
        <div class="order-summary">
          <span>买一 / 卖一</span>
          <strong>
            {formatPrice(props.quote?.bid_price1, props.decimals)} /{" "}
            {formatPrice(props.quote?.ask_price1, props.decimals)}
          </strong>
        </div>
        <div class="order-summary">
          <span>可用资金</span>
          <strong>{formatMoney(props.account?.available)}</strong>
        </div>

        <button
          type="submit"
          class={`submit-order ${side() === "BUY" ? "buy" : "sell"}`}
          disabled={!props.ready || submitting()}
        >
          {submitting() ? "提交中..." : `${side() === "BUY" ? "买入" : "卖出"}${OFFSETS.find((item) => item.value === offset())?.label ?? ""}`}
        </button>
        <p class="ticket-message">{message()}</p>
      </form>

      <Show when={!props.ready}>
        <div class="ticket-lock">node /readyz 未就绪或 WebSocket 断开时禁止报单。</div>
      </Show>
    </aside>
  );
}
