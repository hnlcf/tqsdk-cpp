import { For, Show, createEffect, createMemo, createSignal, onCleanup, onMount } from "solid-js";
import { createMarketStream, getInstruments, type MarketStream, type Quote } from "../api/client";
import { FUTURES_ROWS, type FuturesRow } from "../data/mock";

interface Props {
  onSelect: (symbol: string) => void;
}

export default function FuturesPage(props: Props) {
  const [query, setQuery] = createSignal("");
  const [exchange, setExchange] = createSignal("ALL");
  const [allRows, setAllRows] = createSignal<FuturesRow[]>(FUTURES_ROWS);
  const [live, setLive] = createSignal(false);
  const [quotes, setQuotes] = createSignal<Record<string, Quote>>({});
  const [visibleLimit, setVisibleLimit] = createSignal(100);
  let stream: MarketStream | undefined;

  onMount(async () => {
    stream = createMarketStream({
      onQuote: (next) => setQuotes((previous) => ({ ...previous, ...next })),
    });
    onCleanup(() => stream?.close());
    try {
      const response = await getInstruments({
        classes: "FUTURE,FUTURE_CONT,FUTURE_INDEX",
        limit: 3000,
      });
      if (response.instruments.length > 0) {
        setAllRows(
          response.instruments.map((item) => ({
            symbol: item.instrument_id,
            name: item.name || item.instrument_id,
            exchange: item.exchange_id,
            price: item.last_price || 0,
            changePct: 0,
            volume: item.pre_volume || 0,
            openInterest: item.open_interest || 0,
            marginRate: 0,
          })),
        );
        setLive(true);
      }
    } catch {
      setLive(false);
    }
  });

  const rows = createMemo(() => {
    const q = query().trim().toLowerCase();
    return allRows().filter((row) => {
      const exchangeMatch = exchange() === "ALL" || row.exchange === exchange();
      const queryMatch =
        !q ||
        row.symbol.toLowerCase().includes(q) ||
        row.name.toLowerCase().includes(q);
      return exchangeMatch && queryMatch;
    });
  });
  const visibleRows = createMemo(() => rows().slice(0, visibleLimit()));
  const quoteOf = (symbol: string) => quotes()[symbol];
  const priceOf = (row: FuturesRow) => quoteOf(row.symbol)?.last_price ?? row.price;
  const changeOf = (row: FuturesRow) => {
    const quote = quoteOf(row.symbol);
    if (quote && typeof quote.last_price === "number") {
      const previous = quote.pre_settlement ?? quote.pre_close;
      if (typeof previous === "number" && previous > 0) {
        return ((quote.last_price - previous) / previous) * 100;
      }
    }
    return row.changePct;
  };
  const volumeOf = (row: FuturesRow) => quoteOf(row.symbol)?.volume ?? row.volume;
  const openInterestOf = (row: FuturesRow) =>
    quoteOf(row.symbol)?.open_interest ?? row.openInterest;

  createEffect(() => {
    if (!live() || !stream) return;
    stream.subscribe(visibleRows().map((row) => row.symbol));
  });

  return (
    <section class="page-view">
      <div class="page-heading">
        <div>
          <span class="eyebrow">MARKET SCREENER</span>
          <h1>期货列表 <span class={`source-badge ${live() ? "live" : ""}`}>{live() ? "LIVE" : "MOCK"}</span></h1>
        </div>
        <div class="page-heading-stats">
          <div>
            <small>合约数</small>
            <strong>{rows().length}</strong>
          </div>
          <div>
            <small>有行情</small>
            <strong class="positive">{rows().filter((row) => row.price > 0).length}</strong>
          </div>
          <div>
            <small>无行情</small>
            <strong class="muted">{rows().filter((row) => row.price <= 0).length}</strong>
          </div>
        </div>
      </div>

      <div class="filter-bar">
        <label class="search-field">
          <span>搜索</span>
          <input
            type="search"
            placeholder="合约代码 / 名称"
            value={query()}
            onInput={(event) => setQuery(event.currentTarget.value)}
          />
        </label>
        <div class="exchange-tabs">
          <For each={["ALL", "SHFE", "DCE", "CZCE", "CFFEX", "INE"]}>
            {(item) => (
              <button
                type="button"
                class={exchange() === item ? "active" : ""}
                onClick={() => setExchange(item)}
              >
                {item === "ALL" ? "全部" : item}
              </button>
            )}
          </For>
        </div>
        <span class="filter-hint">点击行进入交易终端</span>
      </div>

      <div class="panel data-panel">
        <table class="market-table">
          <thead>
            <tr>
              <th>合约</th>
              <th>名称</th>
              <th>交易所</th>
              <th>最新价</th>
              <th>涨跌幅</th>
              <th>成交量</th>
              <th>持仓量</th>
              <th>保证金率</th>
            </tr>
          </thead>
          <tbody>
            <For each={visibleRows()}>
              {(row) => (
                <tr class="clickable-row" onClick={() => props.onSelect(row.symbol)}>
                  <td class="mono strong">{row.symbol}</td>
                  <td>{row.name}</td>
                  <td>
                    <span class="exchange-tag">{row.exchange}</span>
                  </td>
                  <td class="mono">{priceOf(row) > 0 ? priceOf(row).toLocaleString("zh-CN") : "—"}</td>
                  <td class={priceOf(row) > 0 ? (changeOf(row) >= 0 ? "positive" : "negative") : "muted"}>
                    {priceOf(row) > 0 ? `${changeOf(row) >= 0 ? "+" : ""}${changeOf(row).toFixed(2)}%` : "—"}
                  </td>
                  <td class="mono">{volumeOf(row) > 0 ? volumeOf(row).toLocaleString("zh-CN") : "—"}</td>
                  <td class="mono">{openInterestOf(row) > 0 ? openInterestOf(row).toLocaleString("zh-CN") : "—"}</td>
                  <td class="mono">{row.marginRate > 0 ? `${(row.marginRate * 100).toFixed(0)}%` : "—"}</td>
                </tr>
              )}
            </For>
          </tbody>
        </table>
        {rows().length === 0 && <div class="empty-state">没有匹配的合约</div>}
        <Show when={rows().length > visibleRows().length}>
          <div class="load-more-row">
            <button type="button" class="load-more" onClick={() => setVisibleLimit((value) => value + 100)}>
              加载更多（{visibleRows().length}/{rows().length}）
            </button>
          </div>
        </Show>
      </div>
    </section>
  );
}
