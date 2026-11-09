import { For, Show, createMemo } from "solid-js";
import type { Quote } from "../api/client";
import { formatPrice, formatVolume } from "../lib/format";

interface Props {
  quote?: Quote;
  decimals: number;
  onPrice: (price: number) => void;
}

interface Level {
  side: "ask" | "bid";
  level: number;
  price: number | null | undefined;
  volume: number | null | undefined;
}

export default function OrderBookPanel(props: Props) {
  const levels = createMemo<Level[]>(() => {
    const quote = props.quote;
    const rows: Level[] = [];
    for (let level = 5; level >= 1; level--) {
      rows.push({
        side: "ask",
        level,
        price: quote?.[`ask_price${level}` as keyof Quote] as number | null | undefined,
        volume: quote?.[`ask_volume${level}` as keyof Quote] as number | null | undefined,
      });
    }
    for (let level = 1; level <= 5; level++) {
      rows.push({
        side: "bid",
        level,
        price: quote?.[`bid_price${level}` as keyof Quote] as number | null | undefined,
        volume: quote?.[`bid_volume${level}` as keyof Quote] as number | null | undefined,
      });
    }
    return rows;
  });
  const maxVolume = createMemo(() =>
    Math.max(1, ...levels().map((row) => Number(row.volume) || 0)),
  );

  return (
    <section class="panel depth-panel">
      <div class="panel-heading compact">
        <div>
          <span class="eyebrow">ORDER BOOK</span>
          <h2>盘口</h2>
        </div>
        <span class="muted">{props.quote?.instrument_id ?? "—"}</span>
      </div>
      <div class="depth-head">
        <span>档位</span>
        <span>价格</span>
        <span>手数</span>
      </div>
      <div class="depth-rows">
        <For each={levels()}>
          {(row) => (
            <button
              type="button"
              class={`depth-row ${row.side}`}
              onClick={() => {
                if (typeof row.price === "number" && row.price > 0) props.onPrice(row.price);
              }}
            >
              <span class="depth-level">{row.side === "ask" ? `卖${row.level}` : `买${row.level}`}</span>
              <span class="depth-price">{formatPrice(row.price, props.decimals)}</span>
              <span class="depth-volume">
                <span
                  class="depth-bar"
                  style={{ width: `${Math.min(100, ((Number(row.volume) || 0) / maxVolume()) * 100)}%` }}
                />
                <span>{formatVolume(row.volume)}</span>
              </span>
            </button>
          )}
        </For>
      </div>
      <Show when={!props.quote}>
        <div class="empty-state small">等待行情</div>
      </Show>
    </section>
  );
}
