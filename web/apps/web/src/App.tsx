import { For, Match, Switch, createEffect, createSignal, onCleanup, onMount } from "solid-js";
import KLineChart from "./chart/KLineChart";
import {
  createMarketStream,
  getKline,
  getQuote,
  type KlineBar,
  type MarketStream,
  type Quote,
} from "./api/client";
import FuturesPage from "./pages/FuturesPage";
import OptionsPage from "./pages/OptionsPage";
import {
  MOCK_ORDERS,
  MOCK_POSITIONS,
  WATCHLIST,
  createMockBars,
  nextTick,
  quoteFromBars,
  type Bar,
  type OrderRow,
  type Position,
} from "./data/mock";

const PERIODS = ["1m", "5m", "15m", "1h", "1d"];
const NAV_ITEMS = [
  { id: "trade", label: "交易终端" },
  { id: "futures", label: "期货列表" },
  { id: "options", label: "期权链" },
] as const;

type View = (typeof NAV_ITEMS)[number]["id"] | "research";
type Theme = "dark" | "light";

function toBars(rows: KlineBar[]): Bar[] {
  return rows
    .filter((row) => Number.isFinite(row.datetime) && row.datetime > 0)
    .map((row) => ({
      time: Math.floor(row.datetime / 1_000_000_000),
      open: row.open,
      high: row.high,
      low: row.low,
      close: row.close,
      volume: row.volume,
    }));
}

function finitePrice(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value) && value > 0;
}

function readInitialView(): View {
  const value = new URLSearchParams(window.location.search).get("view");
  return value === "futures" || value === "options" ? value : "trade";
}

function readInitialTheme(): Theme {
  try {
    const query = new URLSearchParams(window.location.search).get("theme");
    if (query === "dark" || query === "light") return query;
    const saved = window.localStorage.getItem("tqsdk.theme");
    if (saved === "dark" || saved === "light") return saved;
    return window.matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";
  } catch {
    return "dark";
  }
}

export default function App() {
  const [selected, setSelected] = createSignal(WATCHLIST[0].symbol);
  const [period, setPeriod] = createSignal("1m");
  const [bars, setBars] = createSignal<Bar[]>(createMockBars());
  const [side, setSide] = createSignal<"BUY" | "SELL">("BUY");
  const [offset, setOffset] = createSignal<"OPEN" | "CLOSE">("OPEN");
  const [price, setPrice] = createSignal(2993);
  const [volume, setVolume] = createSignal(1);
  const [ticketMsg, setTicketMsg] = createSignal("模拟模式 · 不会发送到柜台");
  const [theme, setTheme] = createSignal<Theme>(readInitialTheme());
  const [view, setView] = createSignal<View>(readInitialView());
  const [liveQuote, setLiveQuote] = createSignal<Quote>();
  const [liveBars, setLiveBars] = createSignal<Bar[]>([]);
  const [streamStatus, setStreamStatus] = createSignal<"connecting" | "online" | "offline">("connecting");
  const [mounted, setMounted] = createSignal(false);
  let stream: MarketStream | undefined;
  let loadGeneration = 0;

  const activeMeta = () => WATCHLIST.find((item) => item.symbol === selected()) ?? WATCHLIST[0];
  const chartBars = () => (liveBars().length > 0 ? liveBars() : bars());
  const last = () => chartBars()[chartBars().length - 1];
  const mockQuote = () => quoteFromBars(selected(), activeMeta().name, chartBars());
  const quote = () => {
    const live = liveQuote();
    if (live && finitePrice(live.last_price)) {
      const previous = finitePrice(live.pre_settlement)
        ? live.pre_settlement
        : finitePrice(live.pre_close)
          ? live.pre_close
          : live.last_price;
      const change = live.last_price - previous;
      return {
        symbol: live.instrument_id,
        name: activeMeta().name,
        price: live.last_price,
        change,
        changePct: previous > 0 ? (change / previous) * 100 : 0,
        bid: finitePrice(live.bid_price1) ? live.bid_price1 : live.last_price - 1,
        ask: finitePrice(live.ask_price1) ? live.ask_price1 : live.last_price + 1,
        volume: live.volume ?? 0,
        openInterest: live.open_interest ?? 0,
      };
    }
    return mockQuote();
  };
  const positions = (): Position[] => MOCK_POSITIONS;
  const orders = (): OrderRow[] => MOCK_ORDERS;
  const watchPrice = (symbol: string, fallback: number) =>
    symbol === selected() ? quote().price : fallback;
  const watchChange = (symbol: string, fallback: number) =>
    symbol === selected() ? quote().changePct : fallback;
  const isLive = () => streamStatus() === "online" && finitePrice(liveQuote()?.last_price);

  createEffect(() => {
    setPrice(Math.round(quote().price));
  });

  createEffect(() => {
    document.documentElement.dataset.theme = theme();
    try {
      window.localStorage.setItem("tqsdk.theme", theme());
    } catch {
      // localStorage can be unavailable in privacy mode; theme still works for this session.
    }
  });

  createEffect(() => {
    const url = new URL(window.location.href);
    if (view() === "trade") {
      url.searchParams.delete("view");
    } else {
      url.searchParams.set("view", view());
    }
    window.history.replaceState(null, "", url);
  });

  function openSymbol(symbol: string) {
    setSelected(symbol);
    setView("trade");
  }

  onMount(() => {
    setMounted(true);
    stream = createMarketStream({
      onStatus: (status) => setStreamStatus(status),
      onQuote: (quotes) => {
        const next = quotes[selected()];
        if (next) setLiveQuote(next);
      },
      onKline: (symbol, duration, payload) => {
        if (symbol !== selected() || duration !== "60000000000") return;
        const update = payload as { reset?: boolean; bars?: KlineBar[] };
        const incoming = toBars(update.bars ?? []);
        if (incoming.length === 0) return;
        if (update.reset) {
          setLiveBars(incoming);
          return;
        }
        setLiveBars((previous) => {
          const next = previous.slice();
          for (const bar of incoming) {
            const index = next.findIndex((item) => item.time === bar.time);
            if (index >= 0) next[index] = bar;
            else next.push(bar);
          }
          next.sort((a, b) => a.time - b.time);
          return next;
        });
      },
    });

    const timer = window.setInterval(() => {
      setBars((previous) => {
        const next = previous.slice();
        const tail = next[next.length - 1];
        next[next.length - 1] = nextTick(tail);
        return next;
      });
    }, 1200);
    onCleanup(() => {
      window.clearInterval(timer);
      stream?.close();
    });
  });

  createEffect(() => {
    if (!mounted()) return;
    const symbol = selected();
    void loadMarket(symbol);
    stream?.subscribe([symbol], [{ symbol, duration_ns: 60_000_000_000 }]);
  });

  async function loadMarket(symbol: string) {
    const generation = ++loadGeneration;
    try {
      const [quoteResponse, klineResponse] = await Promise.all([
        getQuote(symbol),
        getKline(symbol, 60_000_000_000),
      ]);
      if (generation !== loadGeneration) return;
      setLiveQuote(quoteResponse);
      const nextBars = toBars(klineResponse.bars);
      if (nextBars.length > 0) setLiveBars(nextBars);
    } catch {
      if (generation !== loadGeneration) return;
      setLiveQuote(undefined);
      setLiveBars([]);
    }
  }

  function submitDemoOrder(event: SubmitEvent) {
    event.preventDefault();
    setTicketMsg(
      `模拟${side() === "BUY" ? "买入" : "卖出"} ${selected()} ${offset() === "OPEN" ? "开仓" : "平仓"} ${volume()} 手 @ ${price()}`,
    );
  }

  return (
    <div class="terminal">
      <header class="topbar">
        <div class="brand">
          <span class="brand-mark">TQ</span>
          <div>
            <strong>TQSDK Terminal</strong>
            <small>Futures workstation · Live market</small>
          </div>
        </div>

        <nav class="main-nav" aria-label="主导航">
          <For each={NAV_ITEMS}>
            {(item) => (
              <button
                type="button"
                class={view() === item.id ? "active" : ""}
                onClick={() => setView(item.id)}
              >
                {item.label}
              </button>
            )}
          </For>
          <button type="button" class="disabled" disabled title="预留：研究/资讯">
            研究
          </button>
        </nav>

        <div class="topbar-meta">
          <span class={`status-dot ${isLive() ? "live" : "mock"}`} />
          <span>{isLive() ? "实盘行情" : streamStatus() === "connecting" ? "行情连接中" : "模拟行情"}</span>
          <span class="divider" />
          <span>SHFE / DCE / CZCE</span>
          <span class="clock">{new Date().toLocaleTimeString("zh-CN", { hour12: false })}</span>
        </div>
        <div class="account-strip">
          <div>
            <small>权益</small>
            <strong>916,746.38</strong>
          </div>
          <div>
            <small>可用</small>
            <strong>668,810.51</strong>
          </div>
          <div>
            <small>保证金</small>
            <strong>247,552.00</strong>
          </div>
          <div class={quote().change >= 0 ? "positive" : "negative"}>
            <small>浮动盈亏</small>
            <strong>{quote().change >= 0 ? "+" : ""}1,600.00</strong>
          </div>
        </div>
        <button
          type="button"
          class="theme-toggle"
          onClick={() => setTheme(theme() === "dark" ? "light" : "dark")}
          title="切换明暗主题"
        >
          {theme() === "dark" ? "浅色" : "深色"}
        </button>
      </header>

      <Switch>
        <Match when={view() === "trade"}>
          <main class="workspace">
        <aside class="panel watchlist">
          <div class="panel-heading">
            <div>
              <span class="eyebrow">MARKETS</span>
              <h2>自选合约</h2>
            </div>
            <button class="icon-button" type="button" title="添加合约">
              +
            </button>
          </div>
          <div class="watchlist-items">
            <For each={WATCHLIST}>
              {(item) => (
                <button
                  type="button"
                  class={`watch-row ${selected() === item.symbol ? "active" : ""}`}
                  onClick={() => setSelected(item.symbol)}
                >
                  <span class="watch-main">
                    <strong>{item.symbol}</strong>
                    <small>{item.name}</small>
                  </span>
                  <span class="watch-price">
                    <strong>{watchPrice(item.symbol, item.price).toFixed(1)}</strong>
                    <small class={watchChange(item.symbol, item.changePct) >= 0 ? "positive" : "negative"}>
                      {watchChange(item.symbol, item.changePct) >= 0 ? "+" : ""}
                      {watchChange(item.symbol, item.changePct).toFixed(2)}%
                    </small>
                  </span>
                </button>
              )}
            </For>
          </div>
          <div class="session-note">
            <span class="session-bar" />
            <div>
              <strong>日盘交易中</strong>
              <small>行情为本地 mock，下一阶段接 /stream</small>
            </div>
          </div>
        </aside>

        <section class="center-column">
          <div class="chart-header">
            <div class="symbol-title">
              <span class="exchange-badge">{selected().split(".")[0]}</span>
              <div>
                <h1>{selected()}</h1>
                <small>{activeMeta().name}</small>
              </div>
            </div>
            <div class="quote-block">
              <strong class={quote().change >= 0 ? "positive" : "negative"}>
                {quote().price.toFixed(1)}
              </strong>
              <span class={quote().change >= 0 ? "positive" : "negative"}>
                {quote().change >= 0 ? "+" : ""}
                {quote().change.toFixed(1)} / {quote().changePct.toFixed(2)}%
              </span>
            </div>
            <div class="quote-stats">
              <div>
                <small>买一</small>
                <strong class="positive">{quote().bid.toFixed(1)}</strong>
              </div>
              <div>
                <small>卖一</small>
                <strong class="negative">{quote().ask.toFixed(1)}</strong>
              </div>
              <div>
                <small>成交量</small>
                <strong>{quote().volume.toLocaleString("zh-CN")}</strong>
              </div>
              <div>
                <small>持仓量</small>
                <strong>{quote().openInterest.toLocaleString("zh-CN")}</strong>
              </div>
            </div>
          </div>

          <div class="chart-toolbar">
            <div class="period-tabs">
              <For each={PERIODS}>
                {(item) => (
                  <button
                    type="button"
                    class={period() === item ? "active" : ""}
                    onClick={() => setPeriod(item)}
                  >
                    {item}
                  </button>
                )}
              </For>
            </div>
            <div class="toolbar-tools">
              <button type="button" class="tool-button">
                指标
              </button>
              <button type="button" class="tool-button">
                画线
              </button>
              <button type="button" class="tool-button">
                复权
              </button>
            </div>
          </div>

          <div class="chart-shell">
            <KLineChart bars={chartBars()} last={last()} symbol={selected()} theme={theme()} />
          </div>

          <div class="chart-footer">
            <span>O {last().open.toFixed(1)}</span>
            <span>H {last().high.toFixed(1)}</span>
            <span>L {last().low.toFixed(1)}</span>
            <span>C {last().close.toFixed(1)}</span>
            <span>VOL {last().volume.toLocaleString("zh-CN")}</span>
            <span class="chart-credit">Chart by TradingView Lightweight Charts</span>
          </div>
        </section>

        <aside class="panel ticket">
          <div class="panel-heading">
            <div>
              <span class="eyebrow">ORDER ENTRY</span>
              <h2>模拟下单</h2>
            </div>
            <span class="demo-pill">DEMO</span>
          </div>

          <form onSubmit={submitDemoOrder}>
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
              <select value={offset()} onChange={(event) => setOffset(event.currentTarget.value as "OPEN" | "CLOSE")}>
                <option value="OPEN">开仓</option>
                <option value="CLOSE">平仓</option>
              </select>
            </label>

            <label class="field">
              <span>价格</span>
              <div class="input-with-step">
                <button type="button" onClick={() => setPrice((value) => value - 1)}>
                  −
                </button>
                <input
                  type="number"
                  step="1"
                  value={price()}
                  onInput={(event) => setPrice(Number(event.currentTarget.value))}
                />
                <button type="button" onClick={() => setPrice((value) => value + 1)}>
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
              <strong>{selected()}</strong>
            </div>
            <div class="order-summary">
              <span>预估保证金</span>
              <strong>{(price() * volume() * 10 * 0.12).toFixed(2)}</strong>
            </div>

            <button type="submit" class={`submit-order ${side() === "BUY" ? "buy" : "sell"}`}>
              {side() === "BUY" ? "买入开仓" : "卖出平仓"}
            </button>
            <p class="ticket-message">{ticketMsg()}</p>
          </form>

          <div class="quick-positions">
            <div class="panel-heading compact">
              <span class="eyebrow">OPEN POSITIONS</span>
              <span>{positions().length}</span>
            </div>
            <For each={positions()}>
              {(position) => (
                <div class="position-card">
                  <div>
                    <strong>{position.symbol}</strong>
                    <small class={position.side === "LONG" ? "positive" : "negative"}>
                      {position.side === "LONG" ? "多" : "空"} {position.volume} 手
                    </small>
                  </div>
                  <div class="position-pnl">
                    <strong class={position.floatProfit >= 0 ? "positive" : "negative"}>
                      {position.floatProfit >= 0 ? "+" : ""}
                      {position.floatProfit.toFixed(2)}
                    </strong>
                    <small>开仓 {position.openPrice}</small>
                  </div>
                </div>
              )}
            </For>
          </div>
        </aside>
      </main>

      <section class="bottom-grid">
        <div class="panel table-panel">
          <div class="panel-heading">
            <div>
              <span class="eyebrow">POSITIONS</span>
              <h2>持仓</h2>
            </div>
            <span class="muted">浮动盈亏</span>
          </div>
          <table>
            <thead>
              <tr>
                <th>合约</th>
                <th>方向</th>
                <th>手数</th>
                <th>开仓价</th>
                <th>最新价</th>
                <th>保证金</th>
                <th>浮动盈亏</th>
              </tr>
            </thead>
            <tbody>
              <For each={positions()}>
                {(position) => (
                  <tr>
                    <td>{position.symbol}</td>
                    <td class={position.side === "LONG" ? "positive" : "negative"}>
                      {position.side === "LONG" ? "多" : "空"}
                    </td>
                    <td>{position.volume}</td>
                    <td>{position.openPrice}</td>
                    <td>{position.lastPrice}</td>
                    <td>{position.margin.toLocaleString("zh-CN")}</td>
                    <td class={position.floatProfit >= 0 ? "positive" : "negative"}>
                      {position.floatProfit >= 0 ? "+" : ""}
                      {position.floatProfit.toFixed(2)}
                    </td>
                  </tr>
                )}
              </For>
            </tbody>
          </table>
        </div>

        <div class="panel table-panel">
          <div class="panel-heading">
            <div>
              <span class="eyebrow">ORDERS</span>
              <h2>当日委托</h2>
            </div>
            <span class="muted">{orders().filter((order) => order.status === "ALIVE").length} 挂单</span>
          </div>
          <table>
            <thead>
              <tr>
                <th>订单号</th>
                <th>合约</th>
                <th>方向</th>
                <th>开平</th>
                <th>价格</th>
                <th>数量</th>
                <th>状态</th>
              </tr>
            </thead>
            <tbody>
              <For each={orders()}>
                {(order) => (
                  <tr>
                    <td class="mono">{order.id}</td>
                    <td>{order.symbol}</td>
                    <td class={order.side === "BUY" ? "positive" : "negative"}>
                      {order.side === "BUY" ? "买" : "卖"}
                    </td>
                    <td>{order.offset === "OPEN" ? "开" : "平"}</td>
                    <td>{order.price.toFixed(1)}</td>
                    <td>{order.volume}</td>
                    <td>
                      <span class={`order-status ${order.status === "ALIVE" ? "alive" : "done"}`}>
                        {order.message}
                      </span>
                    </td>
                  </tr>
                )}
              </For>
            </tbody>
          </table>
        </div>
          </section>
        </Match>
        <Match when={view() === "futures"}>
          <FuturesPage onSelect={openSymbol} />
        </Match>
        <Match when={view() === "options"}>
          <OptionsPage theme={theme()} />
        </Match>
      </Switch>

      <footer class="footer">
        <span>Phase 0 · mock data</span>
        <span>下一步：REST 快照 + WebSocket diff + 指标 Worker</span>
        <span class="mono">SolidJS + Lightweight Charts</span>
      </footer>
    </div>
  );
}
