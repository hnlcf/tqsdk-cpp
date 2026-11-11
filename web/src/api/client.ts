export type Json = Record<string, unknown>;

export interface InstrumentMeta {
  instrument_id: string;
  class: string;
  exchange_id: string;
  name: string;
  product: string;
  product_name: string;
  underlying_symbol: string;
  option_class: string;
  strike_price: number;
  last_price: number;
  pre_volume: number;
  open_interest: number;
  volume_multiple: number;
  price_tick: number;
  price_decs?: number;
  expire_datetime: number;
  expired: boolean;
}

export interface OptionProduct {
  exchange: string;
  product: string;
  product_name: string;
  underlying_symbol: string;
  expire_datetime: number;
  calls: number;
  puts: number;
}

export interface Quote {
  instrument_id: string;
  datetime?: string;
  last_price?: number | null;
  bid_price1?: number | null;
  ask_price1?: number | null;
  bid_price2?: number | null;
  ask_price2?: number | null;
  bid_price3?: number | null;
  ask_price3?: number | null;
  bid_price4?: number | null;
  ask_price4?: number | null;
  bid_price5?: number | null;
  ask_price5?: number | null;
  bid_volume1?: number | null;
  ask_volume1?: number | null;
  bid_volume2?: number | null;
  ask_volume2?: number | null;
  bid_volume3?: number | null;
  ask_volume3?: number | null;
  bid_volume4?: number | null;
  ask_volume4?: number | null;
  bid_volume5?: number | null;
  ask_volume5?: number | null;
  volume?: number;
  open_interest?: number | null;
  open?: number | null;
  highest?: number | null;
  lowest?: number | null;
  average?: number | null;
  pre_close?: number | null;
  pre_settlement?: number | null;
  upper_limit?: number | null;
  lower_limit?: number | null;
  price_tick?: number | null;
  price_decs?: number | null;
  volume_multiple?: number | null;
  instrument_name?: string;
}

export interface KlineBar {
  datetime: number;
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
  open_oi?: number;
  close_oi?: number;
}

export interface OptionChainItem extends InstrumentMeta {
  quote: Quote;
}

export interface OptionChain {
  underlying: string;
  underlying_contract: InstrumentMeta;
  underlying_quote: Quote;
  expire_datetime: number;
  product: string;
  product_name: string;
  calls: OptionChainItem[];
  puts: OptionChainItem[];
}

export interface OptionGreeks {
  delta: number;
  gamma: number;
  vega: number;
  theta: number;
  rho: number;
}

export interface OptionAnalyticsRow {
  instrument_id: string;
  right: "CALL" | "PUT";
  strike: number;
  time_to_expiry: number;
  market_price: number | null;
  implied_volatility: number | null;
  price: number;
  intrinsic: number;
  time_value: number;
  greeks: OptionGreeks;
}

export interface OptionAnalytics {
  underlying: string;
  spot: number;
  expire_datetime: number;
  time_to_expiry: number;
  rate: number;
  dividend: number;
  vol: number;
  atm_strike: number;
  rows: OptionAnalyticsRow[];
}

export interface CatalogStatus {
  enabled: boolean;
  loaded: boolean;
  source: string;
  total: number;
  active: number;
  futures: number;
  options: number;
  file?: string;
  loaded_at?: number;
  url?: string;
  refresh_interval_sec?: number;
}

export interface ReadyState {
  ready: boolean;
  started?: boolean;
  closed?: boolean;
  md?: {
    present?: boolean;
    connected?: boolean;
    ready?: boolean;
  };
  td?: {
    present?: boolean;
    connected?: boolean;
    ready?: boolean;
    phase?: string;
    expected_disconnect?: boolean;
  };
}

export interface AccountSnapshot {
  balance?: number | null;
  available?: number | null;
  margin?: number | null;
  float_profit?: number | null;
  close_profit?: number | null;
  commission?: number | null;
  risk_ratio?: number | null;
  currency?: string;
}

export interface AccountInfo {
  account_id: string;
  ready: boolean;
  account: AccountSnapshot;
}

export interface AccountsResponse {
  default: string;
  accounts: AccountInfo[];
}

export interface TickCacheStatus {
  enabled: boolean;
  ready: boolean;
  writer_failed?: boolean;
  file?: string;
  max_ticks_per_series?: number;
  accepted?: number;
  written?: number;
  dropped?: number;
  pending?: number;
  flushes?: number;
  last_error?: string;
}

export interface PositionSnapshot {
  instrument_id: string;
  exchange_id?: string;
  volume_long?: number;
  volume_short?: number;
  volume_long_today?: number;
  volume_short_today?: number;
  volume_long_his?: number;
  volume_short_his?: number;
  open_price_long?: number | null;
  open_price_short?: number | null;
  position_price_long?: number | null;
  position_price_short?: number | null;
  last_price?: number | null;
  margin?: number | null;
  float_profit_long?: number | null;
  float_profit_short?: number | null;
  float_profit?: number | null;
  margin_long?: number | null;
  margin_short?: number | null;
  volume_multiple?: number | null;
  price_tick?: number | null;
  price_decs?: number | null;
  instrument_name?: string;
}

export interface OrderSnapshot {
  order_id: string;
  instrument_id: string;
  exchange_id?: string;
  direction?: string;
  offset?: string;
  status?: string;
  last_msg?: string;
  limit_price?: number | null;
  volume_orign?: number;
  volume_left?: number;
  is_dead?: boolean;
  is_error?: boolean;
  insert_date_time?: number;
  trade_price?: number | null;
}

export interface TradeSnapshot {
  trade_id: string;
  order_id?: string;
  instrument_id: string;
  exchange_id?: string;
  direction?: string;
  offset?: string;
  price?: number | null;
  volume?: number;
  trade_date_time?: number;
}

export interface TradeData {
  account?: AccountSnapshot;
  positions?: Record<string, PositionSnapshot>;
  orders?: Record<string, OrderSnapshot>;
  trades?: Record<string, TradeSnapshot>;
}

export interface TradingSnapshot extends TradeData {
  account?: AccountSnapshot;
}

export interface MarketHandlers {
  onStatus?: (status: "connecting" | "online" | "offline") => void;
  onQuote?: (quotes: Record<string, Quote>) => void;
  onKline?: (symbol: string, durationNs: string, payload: unknown) => void;
  onTradeData?: (data: TradeData, snapshot: boolean) => void;
}

const RAW_BASE = (import.meta.env.VITE_API_BASE as string | undefined) ?? "/api";
export const API_BASE = RAW_BASE.replace(/\/$/, "");
const API_TOKEN = (import.meta.env.VITE_API_TOKEN as string | undefined) ?? "";

function authHeaders(): Record<string, string> {
  return API_TOKEN ? { Authorization: `Bearer ${API_TOKEN}` } : {};
}

function wsUrl(account?: string): string {
  let url: string;
  if (API_BASE.startsWith("http://") || API_BASE.startsWith("https://")) {
    url = API_BASE.replace(/^http/, "ws") + "/stream";
  } else {
    const scheme = window.location.protocol === "https:" ? "wss:" : "ws:";
    url = `${scheme}//${window.location.host}${API_BASE}/stream`;
  }
  const query = new URLSearchParams();
  if (account) query.set("account", account);
  if (API_TOKEN) query.set("token", API_TOKEN);
  const suffix = query.toString();
  return suffix ? `${url}?${suffix}` : url;
}

async function requestJson<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(`${API_BASE}${path}`, {
    ...init,
    headers: {
      Accept: "application/json",
      ...authHeaders(),
      ...(init?.headers ?? {}),
    },
  });
  const text = await response.text();
  let body: unknown;
  if (text) {
    try {
      body = JSON.parse(text);
    } catch {
      body = { error: text };
    }
  }
  if (!response.ok) {
    const message =
      body && typeof body === "object" && "error" in body
        ? String((body as { error?: unknown }).error)
        : `${response.status} ${response.statusText}`;
    throw new Error(message);
  }
  return body as T;
}

export async function getReadyz(signal?: AbortSignal): Promise<ReadyState> {
  const response = await fetch(`${API_BASE}/readyz`, {
    signal,
    headers: { Accept: "application/json", ...authHeaders() },
  });
  const text = await response.text();
  if (text) {
    try {
      return JSON.parse(text) as ReadyState;
    } catch {
      // fall through to a useful error
    }
  }
  if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
  return { ready: false };
}

export function getMetrics(signal?: AbortSignal) {
  return requestJson<Record<string, unknown>>("/metrics", { signal });
}

export function getAccounts(signal?: AbortSignal) {
  return requestJson<AccountsResponse>("/accounts", { signal });
}

export function getTickCacheStatus(signal?: AbortSignal) {
  return requestJson<TickCacheStatus>("/tick-cache/status", { signal });
}

export function getKlineCacheStatus(signal?: AbortSignal) {
  return requestJson<Record<string, unknown>>("/kline-cache/status", { signal });
}

export function getCatalogStatus(signal?: AbortSignal) {
  return requestJson<CatalogStatus>("/catalog/status", { signal });
}

export function getInstruments(
  params: {
    classes?: string;
    exchange?: string;
    product?: string;
    q?: string;
    underlying?: string;
    limit?: number;
    offset?: number;
  } = {},
  signal?: AbortSignal,
) {
  const query = new URLSearchParams();
  if (params.classes) query.set("classes", params.classes);
  if (params.exchange) query.set("exchange", params.exchange);
  if (params.product) query.set("product", params.product);
  if (params.q) query.set("q", params.q);
  if (params.underlying) query.set("underlying", params.underlying);
  query.set("limit", String(params.limit ?? 2000));
  if (params.offset) query.set("offset", String(params.offset));
  return requestJson<{ instruments: InstrumentMeta[]; count: number; catalog: CatalogStatus }>(
    `/catalog/instruments?${query.toString()}`,
    { signal },
  );
}

export function getOptionProducts(signal?: AbortSignal) {
  return requestJson<{ products: OptionProduct[]; catalog: CatalogStatus }>(
    "/catalog/options/products",
    { signal },
  );
}

export function getOptionChain(underlying: string, signal?: AbortSignal) {
  return requestJson<OptionChain>(
    `/catalog/option-chain/${encodeURIComponent(underlying)}`,
    { signal },
  );
}

export async function getOptionAnalytics(
  underlying: string,
  params: { spot?: number; rate?: number; dividend?: number; vol?: number; window?: number } = {},
  signal?: AbortSignal,
): Promise<OptionAnalytics> {
  const query = new URLSearchParams();
  if (params.spot !== undefined && Number.isFinite(params.spot)) query.set("spot", String(params.spot));
  if (params.rate !== undefined) query.set("rate", String(params.rate));
  if (params.dividend !== undefined) query.set("dividend", String(params.dividend));
  if (params.vol !== undefined) query.set("vol", String(params.vol));
  if (params.window !== undefined) query.set("window", String(params.window));
  const suffix = query.toString() ? `?${query.toString()}` : "";
  return requestJson<OptionAnalytics>(
    `/option/analytics/${encodeURIComponent(underlying)}${suffix}`,
    { signal },
  );
}

export function getQuote(symbol: string, signal?: AbortSignal) {
  return requestJson<Quote>(`/quote/${encodeURIComponent(symbol)}`, { signal });
}

export function getQuotes(symbols: string[], signal?: AbortSignal) {
  const query = new URLSearchParams({ symbols: symbols.join(",") });
  return requestJson<{ quotes: Record<string, Quote> }>(`/quotes?${query.toString()}`, {
    signal,
  });
}

export function getKline(symbol: string, durationNs: number, signal?: AbortSignal) {
  return requestJson<{ symbol: string; duration_ns: number; bars: KlineBar[] }>(
    `/kline/${encodeURIComponent(symbol)}/${durationNs}`,
    { signal },
  );
}

function accountPath(path: string, account?: string): string {
  if (!account) return path;
  return `${path}${path.includes("?") ? "&" : "?"}account=${encodeURIComponent(account)}`;
}

export function getAccount(signal?: AbortSignal, account?: string) {
  return requestJson<AccountSnapshot>(accountPath("/account", account), { signal });
}

export function getTradingSnapshot(signal?: AbortSignal, account?: string) {
  return requestJson<TradingSnapshot>(accountPath("/trading-snapshot", account), { signal });
}

export function getPositions(signal?: AbortSignal, account?: string) {
  return requestJson<{ positions: Record<string, PositionSnapshot> }>(
    accountPath("/positions", account),
    { signal },
  );
}

export function getOrders(signal?: AbortSignal, account?: string) {
  return requestJson<{ orders: Record<string, OrderSnapshot> }>(accountPath("/orders", account), {
    signal,
  });
}

export function getTrades(signal?: AbortSignal, account?: string) {
  return requestJson<{ trades: Record<string, TradeSnapshot> }>(accountPath("/trades", account), {
    signal,
  });
}

export interface InsertOrderRequest {
  instrument_id: string;
  exchange_id?: string;
  direction: "BUY" | "SELL";
  offset: "OPEN" | "CLOSE" | "CLOSETODAY" | "CLOSEYESTERDAY";
  price_type: "LIMIT" | "ANY" | "BEST" | "FIVELEVEL";
  limit_price: number;
  time_cond: "GFD" | "IOC" | "FOK";
  volume_cond: "ANY" | "ALL" | "MIN";
  advanced?: "NONE" | "FAK" | "FOK";
  offset_leg2?: string;
  volume: number;
  min_volume?: number;
}

export async function insertOrder(body: InsertOrderRequest, account?: string) {
  return requestJson<{ success?: boolean; order_id?: string; error?: string }>(
    accountPath("/order", account),
    {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    },
  );
}

export async function cancelOrder(orderId: string, account?: string) {
  return requestJson<{ success?: boolean; order_id?: string; error?: string }>(
    accountPath(`/order/${encodeURIComponent(orderId)}`, account),
    { method: "DELETE" },
  );
}

export interface KlineSubscription {
  symbol: string;
  duration_ns: number;
  view_width?: number;
}

export interface MarketStream {
  subscribe(symbols: string[], klines?: KlineSubscription[], topics?: string[]): void;
  setSubscription(symbols: string[], klines: KlineSubscription[], topics: string[]): void;
  close(): void;
}

/**
 * WebSocket stream with snapshot/diff handling and reconnect.
 *
 * The C++ node treats a subscribe frame without `topics` as market-data only.
 * This client always sends the four trading topics explicitly for the trading
 * terminal; callers that only need quotes can pass a smaller list.
 */
export function createMarketStream(handlers: MarketHandlers, account?: string): MarketStream {
  let socket: WebSocket | undefined;
  let retry = 0;
  let closed = false;
  let online = false;
  let reconnectTimer: number | undefined;
  let pingTimer: number | undefined;
  const symbols = new Set<string>();
  const klines = new Map<string, KlineSubscription>();
  const topics = new Set<string>();

  const klineKey = (kline: KlineSubscription) => `${kline.symbol}|${kline.duration_ns}`;
  const send = (payload: Record<string, unknown>) => {
    if (socket?.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify(payload));
    }
  };
  const sendSubscribe = () => {
    send({
      op: "subscribe",
      symbols: [...symbols],
      klines: [...klines.values()],
      topics: [...topics],
    });
  };

  const connect = () => {
    if (closed) return;
    handlers.onStatus?.("connecting");
    socket = new WebSocket(wsUrl(account));
    socket.onmessage = (event) => {
      let message: Record<string, unknown>;
      try {
        message = JSON.parse(String(event.data)) as Record<string, unknown>;
      } catch {
        return;
      }
      const data = message.data as Record<string, any> | undefined;
      switch (message.type) {
        case "welcome":
          online = true;
          retry = 0;
          handlers.onStatus?.("online");
          sendSubscribe();
          window.clearInterval(pingTimer);
          pingTimer = window.setInterval(() => send({ op: "ping" }), 20_000);
          break;
        case "snapshot":
          if (data?.quotes) handlers.onQuote?.(data.quotes);
          handlers.onTradeData?.(data ?? {}, true);
          break;
        case "diff":
          if (data?.quotes) handlers.onQuote?.(data.quotes);
          handlers.onTradeData?.(data ?? {}, false);
          if (data?.klines) {
            for (const [symbol, byDuration] of Object.entries(data.klines as object)) {
              for (const [duration, payload] of Object.entries(byDuration as object)) {
                handlers.onKline?.(symbol, duration, payload);
              }
            }
          }
          break;
        default:
          break;
      }
    };
    socket.onclose = () => {
      online = false;
      handlers.onStatus?.("offline");
      window.clearInterval(pingTimer);
      if (!closed) {
        const delay = Math.min(10_000, 300 * 2 ** retry++);
        reconnectTimer = window.setTimeout(connect, delay);
      }
    };
    socket.onerror = () => socket?.close();
  };

  connect();

  return {
    subscribe(nextSymbols, nextKlines = [], nextTopics = []) {
      let changed = false;
      for (const symbol of nextSymbols) {
        if (symbol && !symbols.has(symbol)) {
          symbols.add(symbol);
          changed = true;
        }
      }
      for (const kline of nextKlines) {
        if (!klines.has(klineKey(kline))) {
          klines.set(klineKey(kline), kline);
          changed = true;
        }
      }
      for (const topic of nextTopics) {
        if (!topics.has(topic)) {
          topics.add(topic);
          changed = true;
        }
      }
      if (changed && online) sendSubscribe();
    },
    setSubscription(nextSymbols, nextKlines, nextTopics) {
      const nextSymbolSet = new Set(nextSymbols.filter(Boolean));
      const nextKlineMap = new Map(nextKlines.map((kline) => [klineKey(kline), kline]));
      const nextTopicSet = new Set(nextTopics);
      const releasedSymbols = [...symbols].filter((symbol) => !nextSymbolSet.has(symbol));
      const releasedKlines = [...klines.values()].filter(
        (kline) => !nextKlineMap.has(klineKey(kline)),
      );
      const addedSymbols = [...nextSymbolSet].filter((symbol) => !symbols.has(symbol));
      const addedKlines = [...nextKlineMap.values()].filter(
        (kline) => !klines.has(klineKey(kline)),
      );
      const topicsChanged =
        nextTopicSet.size !== topics.size ||
        [...nextTopicSet].some((topic) => !topics.has(topic));

      symbols.clear();
      for (const symbol of nextSymbolSet) symbols.add(symbol);
      klines.clear();
      for (const [key, kline] of nextKlineMap) klines.set(key, kline);
      topics.clear();
      for (const topic of nextTopicSet) topics.add(topic);

      if (!online) return;
      if (
        !addedSymbols.length &&
        !addedKlines.length &&
        !topicsChanged &&
        !releasedSymbols.length &&
        !releasedKlines.length
      ) {
        return;
      }
      if (releasedSymbols.length || releasedKlines.length) {
        send({
          op: "subscribe",
          release: true,
          symbols: releasedSymbols,
          klines: releasedKlines,
        });
      }
      if (addedSymbols.length || addedKlines.length || topicsChanged) {
        sendSubscribe();
      }
    },
    close() {
      closed = true;
      window.clearTimeout(reconnectTimer);
      window.clearInterval(pingTimer);
      socket?.close();
    },
  };
}
