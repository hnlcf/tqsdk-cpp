import { For, createEffect, createMemo, createSignal, onCleanup, onMount } from "solid-js";
import KLineChart from "../chart/KLineChart";
import {
  createMarketStream,
  getKline,
  getOptionChain,
  getOptionProducts,
  type InstrumentMeta,
  type KlineBar,
  type MarketStream,
  type OptionChain,
  type OptionChainItem,
  type OptionProduct,
  type Quote,
} from "../api/client";
import type { Bar } from "../data/mock";

interface Props {
  theme: "dark" | "light";
}


function toBars(bars: KlineBar[]): Bar[] {
  return bars
    .filter((bar) => Number.isFinite(bar.datetime) && bar.datetime > 0)
    .map((bar) => ({
      time: Math.floor(bar.datetime / 1_000_000_000),
      open: bar.open,
      high: bar.high,
      low: bar.low,
      close: bar.close,
      volume: bar.volume,
    }));
}

function priceOf(quote: Quote | undefined, instrument: InstrumentMeta | undefined): number {
  const value = quote?.last_price;
  if (typeof value === "number" && Number.isFinite(value) && value > 0) {
    return value;
  }
  return instrument?.last_price ?? 0;
}

export default function OptionsPage(props: Props) {
  const [products, setProducts] = createSignal<OptionProduct[]>([]);
  const [exchange, setExchange] = createSignal("");
  const [product, setProduct] = createSignal("");
  const [underlying, setUnderlying] = createSignal("");
  const [chain, setChain] = createSignal<OptionChain>();
  const [selected, setSelected] = createSignal<OptionChainItem[]>([]);
  const [quotes, setQuotes] = createSignal<Record<string, Quote>>({});
  const [optionBars, setOptionBars] = createSignal<Bar[]>([]);
  const [underlyingBars, setUnderlyingBars] = createSignal<Bar[]>([]);
  const [status, setStatus] = createSignal<"loading" | "live" | "mock" | "error">("loading");
  const [loadError, setLoadError] = createSignal("");
  let stream: MarketStream | undefined;

  const exchanges = createMemo(() => [...new Set(products().map((p) => p.exchange))].sort());
  const productsForExchange = createMemo(() =>
    products().filter((p) => !exchange() || p.exchange === exchange()),
  );
  const contracts = createMemo(() =>
    productsForExchange().filter((p) => !product() || p.product === product()),
  );
  const activeProduct = createMemo(
    () => productsForExchange().find((p) => p.product === product()),
  );

  const chainRows = createMemo(() => {
    const calls = new Map<number, OptionChainItem>();
    const puts = new Map<number, OptionChainItem>();
    for (const item of chain()?.calls ?? []) calls.set(item.strike_price, item);
    for (const item of chain()?.puts ?? []) puts.set(item.strike_price, item);
    return [...new Set([...calls.keys(), ...puts.keys()])]
      .sort((a, b) => a - b)
      .map((strike) => ({ strike, call: calls.get(strike), put: puts.get(strike) }));
  });

  const firstSelected = () => selected()[0];
  const quoteOf = (item?: OptionChainItem) => {
    if (!item) return undefined;
    return quotes()[item.instrument_id] ?? item.quote;
  };

  onMount(async () => {
    stream = createMarketStream({
      onQuote: (next) => setQuotes((previous) => ({ ...previous, ...next })),
      onStatus: (next) => {
        if (next === "online") setStatus("live");
      },
    });
    try {
      const response = await getOptionProducts();
      const list = response.products;
      setProducts(list);
      const initial = list.find((p) => p.exchange === "SHFE") ?? list[0];
      if (!initial) {
        setStatus("mock");
        setLoadError("目录中没有可用期权");
        return;
      }
      setExchange(initial.exchange);
      setProduct(initial.product);
      setUnderlying(initial.underlying_symbol);
      setStatus("live");
    } catch (error) {
      setStatus("mock");
      setLoadError(error instanceof Error ? error.message : String(error));
    }
  });

  onCleanup(() => stream?.close());

  createEffect(() => {
    const target = underlying();
    if (!target) return;
    void loadChain(target);
  });

  createEffect(() => {
    const selectedOption = firstSelected();
    if (!selectedOption) {
      setOptionBars([]);
      return;
    }
    void loadOptionKline(selectedOption.instrument_id);
  });

  createEffect(() => {
    const target = chain()?.underlying;
    if (!target) {
      setUnderlyingBars([]);
      return;
    }
    void loadUnderlyingKline(target);
  });

  createEffect(() => {
    if (!stream || !chain()) return;
    const symbols = [
      chain()!.underlying,
      ...chain()!.calls.map((item) => item.instrument_id),
      ...chain()!.puts.map((item) => item.instrument_id),
    ];
    stream.subscribe(symbols, [
      { symbol: chain()!.underlying, duration_ns: 60_000_000_000 },
      ...(firstSelected()
        ? [{ symbol: firstSelected()!.instrument_id, duration_ns: 60_000_000_000 }]
        : []),
    ]);
  });

  async function loadChain(target: string) {
    try {
      setStatus("loading");
      const response = await getOptionChain(target);
      setChain(response);
      setSelected([]);
      const seed: Record<string, Quote> = { [target]: response.underlying_quote };
      for (const item of [...response.calls, ...response.puts]) {
        seed[item.instrument_id] = item.quote;
      }
      setQuotes((previous) => ({ ...previous, ...seed }));
      setStatus("live");
    } catch (error) {
      setStatus("error");
      setLoadError(error instanceof Error ? error.message : String(error));
    }
  }

  async function loadOptionKline(optionSymbol: string) {
    try {
      const response = await getKline(optionSymbol, 60_000_000_000);
      setOptionBars(toBars(response.bars));
    } catch {
      setOptionBars([]);
    }
  }

  async function loadUnderlyingKline(underlyingSymbol: string) {
    try {
      const response = await getKline(underlyingSymbol, 60_000_000_000);
      setUnderlyingBars(toBars(response.bars));
    } catch {
      setUnderlyingBars([]);
    }
  }

  function selectExchange(next: string) {
    setExchange(next);
    const firstProduct = products().find((p) => p.exchange === next);
    if (firstProduct) {
      setProduct(firstProduct.product);
      setUnderlying(firstProduct.underlying_symbol);
    }
  }

  function selectProduct(next: string) {
    setProduct(next);
    const firstContract = products().find(
      (p) => p.exchange === exchange() && p.product === next,
    );
    if (firstContract) setUnderlying(firstContract.underlying_symbol);
  }

  function toggleOption(item: OptionChainItem) {
    setSelected((previous) => {
      if (previous.some((entry) => entry.instrument_id === item.instrument_id)) {
        return previous.filter((entry) => entry.instrument_id !== item.instrument_id);
      }
      return previous.length >= 4 ? previous : [...previous, item];
    });
  }

  function isSelected(item?: OptionChainItem) {
    return !!item && selected().some((entry) => entry.instrument_id === item.instrument_id);
  }

  const payoffPath = createMemo(() => {
    const items = selected();
    const base =
      priceOf(chain()?.underlying_quote, chain()?.underlying_contract) ||
      priceOf(quoteOf(firstSelected()), firstSelected());
    if (items.length === 0 || !base) return "";
    const strikes = items.map((item) => item.strike_price).filter((value) => value > 0);
    const low = Math.min(base, ...strikes) * 0.82;
    const high = Math.max(base, ...strikes) * 1.18;
    if (high <= low) return "";
    const points: Array<[number, number]> = [];
    for (let i = 0; i <= 100; i++) {
      const spot = low + ((high - low) * i) / 100;
      let profit = 0;
      for (const item of items) {
        const premium = priceOf(quoteOf(item), item);
        const intrinsic =
          item.option_class === "CALL"
            ? Math.max(spot - item.strike_price, 0)
            : Math.max(item.strike_price - spot, 0);
        profit += (intrinsic - premium) * (item.volume_multiple || 1);
      }
      points.push([spot, profit]);
    }
    const yValues = points.map(([, y]) => y);
    const yMin = Math.min(0, ...yValues);
    const yMax = Math.max(0, ...yValues);
    const ySpan = yMax - yMin || 1;
    const path = points
      .map(([x, y], index) => {
        const px = ((x - low) / (high - low)) * 620 + 10;
        const py = 160 - ((y - yMin) / ySpan) * 135;
        return `${index === 0 ? "M" : "L"} ${px.toFixed(2)} ${py.toFixed(2)}`;
      })
      .join(" ");
    return path;
  });

  return (
    <section class="page-view">
      <div class="page-heading compact-heading">
        <div>
          <span class="eyebrow">OPTIONS DESK</span>
          <h1>期权链 <span class={`source-badge ${status() === "live" ? "live" : ""}`}>{status().toUpperCase()}</span></h1>
        </div>
        <div class="option-selectors">
          <label>
            <span>交易所</span>
            <select value={exchange()} onChange={(event) => selectExchange(event.currentTarget.value)}>
              <For each={exchanges()}>{(item) => <option value={item}>{item}</option>}</For>
            </select>
          </label>
          <label>
            <span>品种</span>
            <select value={product()} onChange={(event) => selectProduct(event.currentTarget.value)}>
              <For each={[...new Set(productsForExchange().map((p) => p.product))]}>
                {(item) => <option value={item}>{item}</option>}
              </For>
            </select>
          </label>
          <label>
            <span>合约</span>
            <select value={underlying()} onChange={(event) => setUnderlying(event.currentTarget.value)}>
              <For each={contracts()}>
                {(item) => <option value={item.underlying_symbol}>{item.underlying_symbol}</option>}
              </For>
            </select>
          </label>
          <div class="expiry-chip">
            <span>到期</span>
            <strong>{activeProduct()?.expire_datetime ? new Date(activeProduct()!.expire_datetime * 1000).toLocaleDateString("zh-CN") : "—"}</strong>
          </div>
        </div>
      </div>

      <div class="option-toolbar">
        <div class="exchange-tabs">
          <button type="button" class="active">T型报价</button>
          <button type="button">波动率</button>
          <button type="button">希腊字母</button>
        </div>
        <div class="option-summary">
          <span>认沽/认购 <strong>{chain() ? ((chain()!.calls.length / Math.max(1, chain()!.puts.length)) * 100).toFixed(0) : "—"}%</strong></span>
          <span>平值隐波 <strong>—</strong></span>
          <span>总持仓 <strong>{chain() ? (chain()!.calls.length + chain()!.puts.length).toLocaleString("zh-CN") : "—"}</strong></span>
        </div>
        <span class={`source-badge ${status() === "live" ? "live" : ""}`}>{loadError() || (status() === "live" ? "实时行情" : "等待后端")}</span>
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
              <th>选</th>
              <th>最新</th>
              <th>涨跌</th>
              <th class="strike-head">行权价</th>
              <th>涨跌</th>
              <th>最新</th>
              <th>选</th>
            </tr>
          </thead>
          <tbody>
            <For each={chainRows()}>
              {(row) => (
                <tr class={row.strike === 3000 ? "option-atm-row" : ""}>
                  <td><input type="checkbox" checked={isSelected(row.call)} onChange={() => row.call && toggleOption(row.call)} /></td>
                  <td class="mono positive">{priceOf(quoteOf(row.call), row.call).toFixed(2)}</td>
                  <td class={quoteChange(row.call) >= 0 ? "positive" : "negative"}>{formatChange(row.call)}</td>
                  <td class="strike-cell mono">{row.strike}</td>
                  <td class={quoteChange(row.put) >= 0 ? "positive" : "negative"}>{formatChange(row.put)}</td>
                  <td class="mono negative">{priceOf(quoteOf(row.put), row.put).toFixed(2)}</td>
                  <td><input type="checkbox" checked={isSelected(row.put)} onChange={() => row.put && toggleOption(row.put)} /></td>
                </tr>
              )}
            </For>
          </tbody>
        </table>
        {chainRows().length === 0 && <div class="empty-state">{loadError() || "没有可用期权链"}</div>}
      </div>

      <div class="option-analytics">
        <div class="panel payoff-panel">
          <div class="panel-heading">
            <div>
              <span class="eyebrow">PAYOFF</span>
              <h2>组合到期损益</h2>
            </div>
            <span class="muted">{selected().length} 个合约</span>
          </div>
          {selected().length === 0 ? (
            <div class="empty-state">在 T 型报价中勾选期权以生成曲线</div>
          ) : (
            <div class="payoff-chart">
              <svg viewBox="0 0 640 180" preserveAspectRatio="none">
                <line x1="10" y1="90" x2="630" y2="90" class="axis-zero" />
                <path d={payoffPath()} class="payoff-line" />
              </svg>
              <div class="payoff-legend">
                <For each={selected()}>
                  {(item) => (
                    <button type="button" onClick={() => toggleOption(item)}>
                      {item.instrument_id} ×
                    </button>
                  )}
                </For>
              </div>
            </div>
          )}
        </div>
        <div class="panel selected-chart">
          <div class="panel-heading">
            <div>
              <span class="eyebrow">OPTION K-LINE</span>
              <h2>{firstSelected()?.instrument_id ?? "选中期权"}</h2>
            </div>
            <span class="muted">1m</span>
          </div>
          {optionBars().length > 0 ? (
            <div class="mini-chart">
              <KLineChart bars={optionBars()} last={optionBars()[optionBars().length - 1]} symbol={firstSelected()?.instrument_id ?? ""} theme={props.theme} />
            </div>
          ) : (
            <div class="empty-state">勾选一个期权查看 K 线</div>
          )}
        </div>
        <div class="panel selected-chart">
          <div class="panel-heading">
            <div>
              <span class="eyebrow">UNDERLYING K-LINE</span>
              <h2>{chain()?.underlying ?? "标的合约"}</h2>
            </div>
            <span class="muted">1m</span>
          </div>
          {underlyingBars().length > 0 ? (
            <div class="mini-chart">
              <KLineChart bars={underlyingBars()} last={underlyingBars()[underlyingBars().length - 1]} symbol={chain()?.underlying ?? ""} theme={props.theme} />
            </div>
          ) : (
            <div class="empty-state">没有标的 K 线</div>
          )}
        </div>
      </div>

      <div class="option-footnote">
        <span>勾选两侧期权后，组合损益按买入 1 手、到期内在价值减权利金估算。</span>
        <span class="mono">行情来自 /api/stream · 合约来自 symbol.json</span>
      </div>
    </section>
  );

  function quoteChange(item?: OptionChainItem) {
    const quote = quoteOf(item);
    if (!quote || typeof quote.last_price !== "number") return 0;
    const previous = quote.pre_settlement ?? quote.pre_close;
    if (typeof previous !== "number" || previous <= 0) return 0;
    return quote.last_price - previous;
  }

  function formatChange(item?: OptionChainItem) {
    const change = quoteChange(item);
    return `${change >= 0 ? "+" : ""}${change.toFixed(2)}`;
  }
}
