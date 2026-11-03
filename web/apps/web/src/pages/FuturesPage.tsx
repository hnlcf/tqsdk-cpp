import { For, createSignal } from "solid-js";
import { FUTURES_ROWS } from "../data/mock";

interface Props {
  onSelect: (symbol: string) => void;
}

export default function FuturesPage(props: Props) {
  const [query, setQuery] = createSignal("");
  const [exchange, setExchange] = createSignal("ALL");

  const rows = () => {
    const q = query().trim().toLowerCase();
    return FUTURES_ROWS.filter((row) => {
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
          <h1>期货列表</h1>
          <p>合约筛选、行情概览与快速跳转。当前为 mock 数据，下一阶段接 REST/WS。</p>
        </div>
        <div class="page-heading-stats">
          <div>
            <small>合约数</small>
            <strong>{rows().length}</strong>
          </div>
          <div>
            <small>上涨</small>
            <strong class="positive">{rows().filter((row) => row.changePct >= 0).length}</strong>
          </div>
          <div>
            <small>下跌</small>
            <strong class="negative">{rows().filter((row) => row.changePct < 0).length}</strong>
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
                  <td class="mono">{row.price.toLocaleString("zh-CN")}</td>
                  <td class={row.changePct >= 0 ? "positive" : "negative"}>
                    {row.changePct >= 0 ? "+" : ""}
                    {row.changePct.toFixed(2)}%
                  </td>
                  <td class="mono">{row.volume.toLocaleString("zh-CN")}</td>
                  <td class="mono">{row.openInterest.toLocaleString("zh-CN")}</td>
                  <td class="mono">{(row.marginRate * 100).toFixed(0)}%</td>
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
