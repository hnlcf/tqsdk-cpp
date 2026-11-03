import { For, createSignal } from "solid-js";
import { OPTION_EXPIRY, OPTION_ROWS, OPTION_UNDERLYING } from "../data/mock";

export default function OptionsPage() {
  const [view, setView] = createSignal("T型报价");

  return (
    <section class="page-view">
      <div class="page-heading">
        <div>
          <span class="eyebrow">OPTIONS DESK</span>
          <h1>期权链</h1>
          <p>期权 T 型报价、成交量和隐波入口。当前为 mock 数据，布局先按期货客户端习惯预留。</p>
        </div>
        <div class="option-underlying">
          <small>标的</small>
          <strong>{OPTION_UNDERLYING}</strong>
          <span>到期 {OPTION_EXPIRY}</span>
        </div>
      </div>

      <div class="option-toolbar">
        <div class="exchange-tabs">
          <For each={["T型报价", "波动率", "希腊字母"]}>
            {(item) => (
              <button type="button" class={view() === item ? "active" : ""} onClick={() => setView(item)}>
                {item}
              </button>
            )}
          </For>
        </div>
        <div class="option-summary">
          <span>认沽/认购比率 <strong>0.83</strong></span>
          <span>平值隐波 <strong>18.6%</strong></span>
          <span>总持仓 <strong>24,860</strong></span>
        </div>
      </div>

      <div class="panel data-panel option-panel">
        <div class="option-table-title">
          <span class="positive">认购 CALLS</span>
          <span>行权价</span>
          <span class="negative">认沽 PUTS</span>
        </div>
        <table class="option-table">
          <thead>
            <tr>
              <th>最新</th>
              <th>涨跌</th>
              <th>成交量</th>
              <th class="strike-head">行权价</th>
              <th>成交量</th>
              <th>涨跌</th>
              <th>最新</th>
            </tr>
          </thead>
          <tbody>
            <For each={OPTION_ROWS}>
              {(row) => {
                const atm = Math.abs(row.strike - 3000) < 1;
                return (
                  <tr class={atm ? "option-atm-row" : ""}>
                    <td class="mono positive">{row.callLast.toFixed(1)}</td>
                    <td class={row.callChangePct >= 0 ? "positive" : "negative"}>
                      {row.callChangePct >= 0 ? "+" : ""}
                      {row.callChangePct.toFixed(1)}%
                    </td>
                    <td class="mono">{row.callVolume.toLocaleString("zh-CN")}</td>
                    <td class="strike-cell mono">{row.strike}</td>
                    <td class="mono">{row.putVolume.toLocaleString("zh-CN")}</td>
                    <td class={row.putChangePct >= 0 ? "positive" : "negative"}>
                      {row.putChangePct >= 0 ? "+" : ""}
                      {row.putChangePct.toFixed(1)}%
                    </td>
                    <td class="mono negative">{row.putLast.toFixed(1)}</td>
                  </tr>
                );
              }}
            </For>
          </tbody>
        </table>
      </div>

      <div class="option-footnote">
        <span>下一阶段：接入期权合约列表、行权价链、隐波曲面与持仓联动。</span>
        <span class="mono">CFFEX / SHFE / DCE / CZCE</span>
      </div>
    </section>
  );
}
