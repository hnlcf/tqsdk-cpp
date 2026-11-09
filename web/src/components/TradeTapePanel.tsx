import { For, Show } from "solid-js";
import { formatPrice, formatVolume } from "../lib/format";

export interface TapeRow {
  id: number;
  time: string;
  price: number;
  volume: number;
  side: "BUY" | "SELL";
}

interface Props {
  rows: TapeRow[];
  decimals: number;
}

export default function TradeTapePanel(props: Props) {
  return (
    <section class="panel tape-panel">
      <div class="panel-heading compact">
        <div>
          <span class="eyebrow">TIME &amp; SALES</span>
          <h2>分笔成交</h2>
        </div>
        <span class="muted">{props.rows.length} 笔</span>
      </div>
      <div class="tape-head">
        <span>时间</span>
        <span>价格</span>
        <span>手数</span>
      </div>
      <div class="tape-rows">
        <For each={props.rows}>
          {(row) => (
            <div class={`tape-row ${row.side === "BUY" ? "positive" : "negative"}`}>
              <span class="mono">{row.time}</span>
              <span class="mono">{formatPrice(row.price, props.decimals)}</span>
              <span class="mono">{formatVolume(row.volume)}</span>
            </div>
          )}
        </For>
      </div>
      <Show when={props.rows.length === 0}>
        <div class="empty-state small">等待成交</div>
      </Show>
    </section>
  );
}
