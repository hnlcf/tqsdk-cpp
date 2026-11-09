import { For, Show, createSignal } from "solid-js";
import type { Quote } from "../api/client";
import { formatPrice, quoteChange } from "../lib/format";

interface Props {
  symbols: string[];
  quotes: Record<string, Quote>;
  selected: string;
  onSelect: (symbol: string) => void;
  onAdd: (symbol: string) => void;
  onRemove: (symbol: string) => void;
  onMove: (symbol: string, direction: -1 | 1) => void;
}

export default function WatchlistPanel(props: Props) {
  const [entry, setEntry] = createSignal("");
  const [editing, setEditing] = createSignal(false);

  function submit(event: SubmitEvent) {
    event.preventDefault();
    const symbol = entry().trim().toUpperCase();
    if (!symbol) return;
    props.onAdd(symbol);
    setEntry("");
    setEditing(false);
  }

  return (
    <aside class="panel watchlist">
      <div class="panel-heading">
        <div>
          <span class="eyebrow">WATCHLIST</span>
          <h2>自选合约</h2>
        </div>
        <button
          class="icon-button"
          type="button"
          title="添加合约"
          onClick={() => setEditing((value) => !value)}
        >
          {editing() ? "×" : "+"}
        </button>
      </div>

      <Show when={editing()}>
        <form class="watch-add" onSubmit={submit}>
          <input
            autofocus
            value={entry()}
            placeholder="SHFE.rb2701"
            onInput={(event) => setEntry(event.currentTarget.value)}
          />
          <button type="submit" title="加入自选">
            加入
          </button>
        </form>
      </Show>

      <div class="watchlist-items">
        <For each={props.symbols}>
          {(symbol) => {
            const quote = () => props.quotes[symbol];
            const change = () => quoteChange(quote());
            return (
              <div
                class={`watch-row ${props.selected === symbol ? "active" : ""}`}
                onClick={() => props.onSelect(symbol)}
              >
                <span class="watch-main">
                  <strong>{symbol}</strong>
                  <small>{symbol.split(".")[0]} · 实时行情</small>
                </span>
                <span class="watch-price">
                  <strong>{formatPrice(quote()?.last_price, 2)}</strong>
                  <small class={change().pct >= 0 ? "positive" : "negative"}>
                    {change().pct >= 0 ? "+" : ""}
                    {change().pct.toFixed(2)}%
                  </small>
                </span>
                <span class="watch-actions" onClick={(event) => event.stopPropagation()}>
                  <button type="button" title="上移" onClick={() => props.onMove(symbol, -1)}>
                    ↑
                  </button>
                  <button type="button" title="下移" onClick={() => props.onMove(symbol, 1)}>
                    ↓
                  </button>
                  <button type="button" title="移除" onClick={() => props.onRemove(symbol)}>
                    ×
                  </button>
                </span>
              </div>
            );
          }}
        </For>
      </div>

      <div class="session-note">
        <span class="session-bar" />
        <div>
          <strong>REST + WebSocket</strong>
          <small>自选列表持久化到本机浏览器</small>
        </div>
      </div>
    </aside>
  );
}
