import {
  Match,
  Show,
  Switch,
  createEffect,
  createMemo,
  createSignal,
  onCleanup,
  onMount,
  untrack,
} from "solid-js";
import {
  API_BASE,
  cancelOrder,
  createMarketStream,
  getKline,
  getQuote,
  getReadyz,
  getTradingSnapshot,
  insertOrder,
  type AccountSnapshot,
  type KlineBar,
  type MarketStream,
  type OrderSnapshot,
  type PositionSnapshot,
  type Quote,
  type ReadyState,
  type TradeData,
  type TradeSnapshot,
} from "./api/client";
import type { TicketOrder } from "./components/OrderTicketPanel";
import Topbar from "./components/Topbar";
import WatchlistPanel from "./components/WatchlistPanel";
import ChartPanel, { type PeriodOption } from "./components/ChartPanel";
import OrderTicketPanel from "./components/OrderTicketPanel";
import OrderBookPanel from "./components/OrderBookPanel";
import TradeTapePanel, { type TapeRow } from "./components/TradeTapePanel";
import TradingTables from "./components/TradingTables";
import FuturesPage from "./pages/FuturesPage";
import OptionsPage from "./pages/OptionsPage";
import {
  DEFAULT_INDICATORS,
  type IndicatorConfig,
  type OHLCV,
} from "./indicators/indicators";
import { finite, priceDecimals } from "./lib/format";
import {
  readChartMode,
  readIndicators,
  readPeriod,
  readSelected,
  readTheme,
  readWatchlist,
  saveChartMode,
  saveIndicators,
  savePeriod,
  saveSelected,
  saveTheme,
  saveWatchlist,
  type Theme,
  type View,
} from "./lib/storage";

const PERIODS: PeriodOption[] = [
  { id: "1m", label: "1m", durationNs: 60_000_000_000 },
  { id: "5m", label: "5m", durationNs: 300_000_000_000 },
  { id: "15m", label: "15m", durationNs: 900_000_000_000 },
  { id: "30m", label: "30m", durationNs: 1_800_000_000_000 },
  { id: "1h", label: "1h", durationNs: 3_600_000_000_000 },
  { id: "2h", label: "2h", durationNs: 7_200_000_000_000 },
  { id: "4h", label: "4h", durationNs: 14_400_000_000_000 },
  { id: "1d", label: "1d", durationNs: 86_400_000_000_000 },
  { id: "3d", label: "3d", durationNs: 259_200_000_000_000 },
  { id: "1w", label: "1w", durationNs: 604_800_000_000_000 },
];
const TRADING_TOPICS = ["account", "orders", "trades", "positions"];

function toOHLCV(rows: KlineBar[]): OHLCV[] {
  return rows
    .filter((row) => Number.isFinite(row.datetime) && row.datetime > 0)
    .map((row) => ({
      time: Math.floor(row.datetime / 1_000_000_000),
      open: row.open,
      high: row.high,
      low: row.low,
      close: row.close,
      volume: row.volume,
    }))
    .sort((a, b) => a.time - b.time);
}

function mergeBars(previous: OHLCV[], incoming: OHLCV[]): OHLCV[] {
  const map = new Map<number, OHLCV>();
  for (const bar of previous) map.set(bar.time, bar);
  for (const bar of incoming) map.set(bar.time, bar);
  return [...map.values()].sort((a, b) => a.time - b.time).slice(-2000);
}

function initialView(): View {
  const query = new URLSearchParams(window.location.search).get("view");
  return query === "futures" || query === "options" ? query : "terminal";
}

export default function App() {
  const [view, setView] = createSignal<View>(initialView());
  const [theme, setTheme] = createSignal<Theme>(readTheme());
  const [watchlist, setWatchlist] = createSignal<string[]>(readWatchlist());
  const [selected, setSelected] = createSignal(readSelected());
  const [period, setPeriod] = createSignal(readPeriod());
  const [chartMode, setChartMode] = createSignal<"kline" | "time">(readChartMode());
  const [indicators, setIndicators] = createSignal<IndicatorConfig>(
    readIndicators(DEFAULT_INDICATORS),
  );

  const [quotes, setQuotes] = createSignal<Record<string, Quote>>({});
  const [history, setHistory] = createSignal<OHLCV[]>([]);
  const [liveBars, setLiveBars] = createSignal<OHLCV[]>([]);
  const [ready, setReady] = createSignal<ReadyState>();
  const [streamStatus, setStreamStatus] =
    createSignal<"connecting" | "online" | "offline">("connecting");
  const [streamReady, setStreamReady] = createSignal(false);
  const [account, setAccount] = createSignal<AccountSnapshot>();
  const [positions, setPositions] = createSignal<Record<string, PositionSnapshot>>({});
  const [orders, setOrders] = createSignal<Record<string, OrderSnapshot>>({});
  const [trades, setTrades] = createSignal<Record<string, TradeSnapshot>>({});
  const [tape, setTape] = createSignal<TapeRow[]>([]);
  const [ticketPrice, setTicketPrice] = createSignal(0);
  const [toast, setToast] = createSignal("");

  let stream: MarketStream | undefined;
  let loadGeneration = 0;
  let snapshotLoaded = false;
  let tapeSequence = 0;
  let priceSymbol = "";
  const lastVolume = new Map<string, number>();
  const lastPrice = new Map<string, number>();

  const selectedQuote = createMemo(() => quotes()[selected()]);
  const decimals = createMemo(() => priceDecimals(selectedQuote(), 2));
  const periodDuration = createMemo(
    () => PERIODS.find((item) => item.id === period())?.durationNs ?? 60_000_000_000,
  );
  const activeDuration = createMemo(() =>
    chartMode() === "time" ? 60_000_000_000 : periodDuration(),
  );
  const chartBars = createMemo(() =>
    liveBars().length > 0 ? mergeBars(history(), liveBars()) : history(),
  );
  const tradingReady = createMemo(
    () => Boolean(ready()?.ready) && streamStatus() === "online",
  );

  function notify(message: string) {
    setToast(message);
    window.setTimeout(() => setToast((current) => (current === message ? "" : current)), 4000);
  }

  function applyTradeData(data: TradeData, snapshot: boolean) {
    if (data.account) setAccount(data.account);
    if (snapshot) {
      setPositions(data.positions ?? {});
      setOrders(data.orders ?? {});
      setTrades(data.trades ?? {});
      return;
    }
    if (data.positions) setPositions((previous) => ({ ...previous, ...data.positions }));
    if (data.orders) setOrders((previous) => ({ ...previous, ...data.orders }));
    if (data.trades) setTrades((previous) => ({ ...previous, ...data.trades }));
  }

  function recordTape(symbol: string, quote: Quote) {
    const previousVolume = lastVolume.get(symbol);
    const previousPrice = lastPrice.get(symbol);
    if (finite(quote.volume)) lastVolume.set(symbol, quote.volume);
    if (finite(quote.last_price)) lastPrice.set(symbol, quote.last_price);
    if (symbol !== selected() || !finite(quote.volume) || !finite(quote.last_price)) return;
    if (previousVolume === undefined || quote.volume <= previousVolume || quote.last_price <= 0) {
      return;
    }
    const side: "BUY" | "SELL" =
      previousPrice === undefined || quote.last_price >= previousPrice ? "BUY" : "SELL";
    const row: TapeRow = {
      id: ++tapeSequence,
      time: new Date().toLocaleTimeString("zh-CN", { hour12: false }),
      price: quote.last_price,
      volume: quote.volume - previousVolume,
      side,
    };
    setTape((previous) => [row, ...previous].slice(0, 80));
  }

  async function refreshReady() {
    try {
      setReady(await getReadyz());
    } catch {
      setReady(undefined);
    }
  }

  async function loadTradingSnapshot() {
    try {
      applyTradeData(await getTradingSnapshot(), true);
      snapshotLoaded = true;
    } catch {
      snapshotLoaded = false;
    }
  }

  async function loadChart(symbol: string, duration: number) {
    const generation = ++loadGeneration;
    setHistory([]);
    setLiveBars([]);
    try {
      const [quote, kline] = await Promise.all([
        getQuote(symbol),
        getKline(symbol, duration),
      ]);
      if (generation !== loadGeneration) return;
      setQuotes((previous) => ({ ...previous, [symbol]: quote }));
      setHistory(toOHLCV(kline.bars));
    } catch (error) {
      if (generation !== loadGeneration) return;
      notify(error instanceof Error ? error.message : String(error));
    }
  }

  function selectSymbol(symbol: string) {
    setSelected(symbol);
    setView("terminal");
  }

  function addSymbol(input: string) {
    const symbol = input.trim().toUpperCase();
    if (!symbol) return;
    if (!symbol.includes(".")) {
      notify("请输入带交易所的合约，例如 SHFE.rb2701");
      return;
    }
    setWatchlist((previous) => (previous.includes(symbol) ? previous : [...previous, symbol]));
    selectSymbol(symbol);
  }

  function removeSymbol(symbol: string) {
    setWatchlist((previous) => {
      const next = previous.filter((item) => item !== symbol);
      if (next.length === 0) return previous;
      if (symbol === selected()) setSelected(next[0]);
      return next;
    });
  }

  function moveSymbol(symbol: string, direction: -1 | 1) {
    setWatchlist((previous) => {
      const index = previous.indexOf(symbol);
      const target = index + direction;
      if (index < 0 || target < 0 || target >= previous.length) return previous;
      const next = previous.slice();
      [next[index], next[target]] = [next[target], next[index]];
      return next;
    });
  }

  async function submitTicket(order: TicketOrder): Promise<string> {
    if (!tradingReady()) throw new Error("交易网关未就绪");
    const result = await insertOrder({
      instrument_id: selected(),
      exchange_id: selected().split(".")[0],
      direction: order.side,
      offset: order.offset,
      price_type: "LIMIT",
      limit_price: order.price,
      time_cond: "GFD",
      volume_cond: "ANY",
      volume: order.volume,
      min_volume: 0,
    });
    if (!result.success) throw new Error(result.error || "报单失败");
    void refreshReady();
    return result.order_id ? `已提交 ${result.order_id}` : "已提交";
  }

  async function cancelLiveOrder(orderId: string) {
    try {
      const result = await cancelOrder(orderId);
      notify(result.success ? `撤单已提交 ${orderId}` : result.error || "撤单失败");
    } catch (error) {
      notify(error instanceof Error ? error.message : String(error));
    }
  }

  onMount(() => {
    stream = createMarketStream({
      onStatus: (status) => setStreamStatus(status),
      onQuote: (next) => {
        setQuotes((previous) => ({ ...previous, ...next }));
        for (const [symbol, quote] of Object.entries(next)) recordTape(symbol, quote);
      },
      onTradeData: (data, snapshot) => applyTradeData(data, snapshot),
      onKline: (symbol, duration, payload) => {
        if (symbol !== selected() || Number(duration) !== activeDuration()) return;
        const update = payload as { reset?: boolean; bars?: KlineBar[] };
        const incoming = toOHLCV(update.bars ?? []);
        if (incoming.length === 0) return;
        if (update.reset) {
          setLiveBars(incoming);
        } else {
          setLiveBars((previous) => mergeBars(previous, incoming));
        }
      },
    });
    setStreamReady(true);
    void refreshReady();
    void loadTradingSnapshot();
    const timer = window.setInterval(() => {
      void refreshReady();
      if (!snapshotLoaded && ready()?.ready) void loadTradingSnapshot();
    }, 3000);
    onCleanup(() => {
      window.clearInterval(timer);
      stream?.close();
      stream = undefined;
    });
  });

  createEffect(() => {
    if (!streamReady() || !stream) return;
    const symbols = [...new Set([...watchlist(), selected()])];
    stream.setSubscription(
      symbols,
      [{ symbol: selected(), duration_ns: activeDuration(), view_width: 600 }],
      TRADING_TOPICS,
    );
  });

  createEffect(() => {
    const symbol = selected();
    const duration = activeDuration();
    untrack(() => {
      setTape([]);
      lastVolume.clear();
      lastPrice.clear();
      void loadChart(symbol, duration);
    });
  });

  createEffect(() => {
    const symbol = selected();
    const quote = quotes()[symbol];
    if (priceSymbol !== symbol) {
      priceSymbol = symbol;
      setTicketPrice(finite(quote?.last_price) ? quote.last_price : 0);
    } else if (!(untrack(() => ticketPrice()) > 0) && finite(quote?.last_price)) {
      setTicketPrice(quote.last_price);
    }
  });

  createEffect(() => {
    document.documentElement.dataset.theme = theme();
    saveTheme(theme());
  });

  createEffect(() => {
    saveWatchlist(watchlist());
  });

  createEffect(() => {
    saveSelected(selected());
  });

  createEffect(() => {
    savePeriod(period());
  });

  createEffect(() => {
    saveChartMode(chartMode());
  });

  createEffect(() => {
    saveIndicators(indicators());
  });

  createEffect(() => {
    const url = new URL(window.location.href);
    if (view() === "terminal") url.searchParams.delete("view");
    else url.searchParams.set("view", view());
    if (theme() === "dark") url.searchParams.delete("theme");
    else url.searchParams.set("theme", theme());
    window.history.replaceState(null, "", url);
  });

  return (
    <div class="terminal">
      <Topbar
        view={view()}
        theme={theme()}
        streamStatus={streamStatus()}
        ready={ready()}
        account={account()}
        onView={setView}
        onTheme={() => setTheme(theme() === "dark" ? "light" : "dark")}
      />

      <Switch>
        <Match when={view() === "terminal"}>
          <main class="workspace">
            <WatchlistPanel
              symbols={watchlist()}
              quotes={quotes()}
              selected={selected()}
              onSelect={selectSymbol}
              onAdd={addSymbol}
              onRemove={removeSymbol}
              onMove={moveSymbol}
            />
            <ChartPanel
              symbol={selected()}
              quote={selectedQuote()}
              bars={chartBars()}
              period={period()}
              periods={PERIODS}
              mode={chartMode()}
              theme={theme()}
              decimals={decimals()}
              indicators={indicators()}
              onPeriod={(next) => {
                setPeriod(next);
                setChartMode("kline");
              }}
              onMode={setChartMode}
              onIndicators={setIndicators}
            />
            <div class="right-rail">
              <OrderTicketPanel
                symbol={selected()}
                quote={selectedQuote()}
                account={account()}
                ready={tradingReady()}
                decimals={decimals()}
                price={ticketPrice()}
                onPriceChange={setTicketPrice}
                onSubmit={submitTicket}
              />
              <OrderBookPanel
                quote={selectedQuote()}
                decimals={decimals()}
                onPrice={setTicketPrice}
              />
              <TradeTapePanel rows={tape()} decimals={decimals()} />
            </div>
          </main>

          <TradingTables
            account={account()}
            positions={positions()}
            orders={orders()}
            trades={trades()}
            ready={tradingReady()}
            onCancel={cancelLiveOrder}
          />
        </Match>
        <Match when={view() === "futures"}>
          <FuturesPage onSelect={addSymbol} />
        </Match>
        <Match when={view() === "options"}>
          <OptionsPage theme={theme()} />
        </Match>
      </Switch>

      <Show when={toast()}>
        <div class="toast" role="status">
          {toast()}
        </div>
      </Show>

      <footer class="footer">
        <span>{API_BASE}</span>
        <span>
          WS {streamStatus()} · TD {ready()?.td?.phase ?? "unknown"} ·{" "}
          {ready()?.td?.expected_disconnect ? "结算预期断线" : "正常"}
        </span>
        <span class="mono">SolidJS · pnpm · Lightweight Charts</span>
      </footer>
    </div>
  );
}
