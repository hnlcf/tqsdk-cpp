import { For, createSignal, onMount } from "solid-js";
import { getInstruments } from "../api/client";
import { FUTURES_ROWS, type FuturesRow } from "../data/mock";

interface Props {
  onSelect: (symbol: string) => void;
}

export default function FuturesPage(props: Props) {
  const [query, setQuery] = createSignal("");
  const [exchange, setExchange] = createSignal("ALL");
  const [allRows, setAllRows] = createSignal<FuturesRow[]>(FUTURES_ROWS);
  const [live, setLive] = createSignal(false);

  onMount(async () => {
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

  const rows = () => {
    const q = query().trim().toLowerCase();
    return allRows().filter((row) => {
      const exchangeMatch = exchange() === "ALL" || row.exchange === exchange();
      const queryMatch =
        !q ||
        row.symbol.toLowerCase().includes(q) ||
        row.name.toLowerCase().includes(q);
      return exchangeMatch && queryMatch;
    });
  };

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
            <For each={rows()}>
              {(row) => (
                <tr class="clickable-row" onClick={() => props.onSelect(row.symbol)}>
                  <td class="mono strong">{row.symbol}</td>
                  <td>{row.name}</td>
                  <td>
                    <span class="exchange-tag">{row.exchange}</span>
                  </td>
                  <td class="mono">{row.price > 0 ? row.price.toLocaleString("zh-CN") : "—"}</td>
                  <td class={row.price > 0 ? (row.changePct >= 0 ? "positive" : "negative") : "muted"}>
                    {row.price > 0 ? `${row.changePct >= 0 ? "+" : ""}${row.changePct.toFixed(2)}%` : "—"}
                  </td>
                  <td class="mono">{row.volume > 0 ? row.volume.toLocaleString("zh-CN") : "—"}</td>
                  <td class="mono">{row.openInterest > 0 ? row.openInterest.toLocaleString("zh-CN") : "—"}</td>
                  <td class="mono">{row.marginRate > 0 ? `${(row.marginRate * 100).toFixed(0)}%` : "—"}</td>
                </tr>
              )}
            </For>
          </tbody>
        </table>
        {rows().length === 0 && <div class="empty-state">没有匹配的合约</div>}
      </div>
    </section>
  );
}
