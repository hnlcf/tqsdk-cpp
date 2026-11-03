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
  bid_volume1?: number;
  ask_volume1?: number;
  volume?: number;
  open_interest?: number | null;
  pre_close?: number | null;
  pre_settlement?: number | null;
  upper_limit?: number | null;
  lower_limit?: number | null;
  price_tick?: number | null;
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

export interface CatalogStatus {
  enabled: boolean;
  loaded: boolean;
  source: string;
  total: number;
  active: number;
  futures: number;
  options: number;
}

export interface MarketHandlers {
  onStatus?: (status: "connecting" | "online" | "offline") => void;
  onQuote?: (quotes: Record<string, Quote>) => void;
  onKline?: (symbol: string, durationNs: string, payload: unknown) => void;
}

const RAW_BASE = (import.meta.env.VITE_API_BASE as string | undefined) ?? "/api";
export const API_BASE = RAW_BASE.replace(/\/$/, "");
export const isLiveConfigured = API_BASE.length > 0;

function wsUrl(): string {
  if (API_BASE.startsWith("http://") || API_BASE.startsWith("https://")) {
    return API_BASE.replace(/^http/, "ws") + "/stream";
  }
  const scheme = window.location.protocol === "https:" ? "wss:" : "ws:";
  return `${scheme}//${window.location.host}${API_BASE}/stream`;
}

async function getJson<T>(path: string, signal?: AbortSignal): Promise<T> {
  const response = await fetch(`${API_BASE}${path}`, {
    signal,
    headers: { Accept: "application/json" },
  });
  if (!response.ok) {
    throw new Error(`${response.status} ${response.statusText}`);
  }
  return (await response.json()) as T;
}

export function getCatalogStatus(signal?: AbortSignal) {
  return getJson<{ loaded: boolean } & CatalogStatus>("/catalog/status", signal);
}

export function getInstruments(
  params: {
    classes?: string;
    exchange?: string;
    product?: string;
    q?: string;
    underlying?: string;
    limit?: number;
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
  return getJson<{ instruments: InstrumentMeta[]; count: number; catalog: CatalogStatus }>(
    `/catalog/instruments?${query.toString()}`,
    signal,
  );
}

export function getOptionProducts(signal?: AbortSignal) {
  return getJson<{ products: OptionProduct[]; catalog: CatalogStatus }>(
    "/catalog/options/products",
    signal,
  );
}

export function getOptionChain(underlying: string, signal?: AbortSignal) {
  return getJson<OptionChain>(
    `/catalog/option-chain/${encodeURIComponent(underlying)}`,
    signal,
  );
}

export function getQuote(symbol: string, signal?: AbortSignal) {
  return getJson<Quote>(`/quote/${encodeURIComponent(symbol)}`, signal);
}

export function getKline(symbol: string, durationNs: number, signal?: AbortSignal) {
  return getJson<{ symbol: string; duration_ns: number; bars: KlineBar[] }>(
    `/kline/${encodeURIComponent(symbol)}/${durationNs}`,
    signal,
  );
}

export function getAccount(signal?: AbortSignal) {
  return getJson<Record<string, number | string | null>>("/account", signal);
}

export interface MarketStream {
  subscribe(symbols: string[], klines?: Array<{ symbol: string; duration_ns: number }>): void;
  close(): void;
}

export function createMarketStream(handlers: MarketHandlers): MarketStream {
  let socket: WebSocket | undefined;
  let retry = 0;
  let closed = false;
  let online = false;
  let reconnectTimer: number | undefined;
  let pingTimer: number | undefined;
  const symbols = new Set<string>();
  const klines = new Map<string, { symbol: string; duration_ns: number }>();

  const sendSubscribe = () => {
    if (!socket || socket.readyState !== WebSocket.OPEN) return;
    socket.send(
      JSON.stringify({
        op: "subscribe",
        symbols: [...symbols],
        klines: [...klines.values()],
        topics: [],
      }),
    );
  };

  const connect = () => {
    if (closed) return;
    handlers.onStatus?.("connecting");
    socket = new WebSocket(wsUrl());
    socket.onmessage = (event) => {
      let message: Record<string, any>;
      try {
        message = JSON.parse(String(event.data));
      } catch {
        return;
      }
      switch (message.type) {
        case "welcome":
          online = true;
          retry = 0;
          handlers.onStatus?.("online");
          sendSubscribe();
          window.clearInterval(pingTimer);
          pingTimer = window.setInterval(() => {
            if (socket?.readyState === WebSocket.OPEN) {
              socket.send(JSON.stringify({ op: "ping" }));
            }
          }, 20_000);
          break;
        case "snapshot":
          if (message.data?.quotes) handlers.onQuote?.(message.data.quotes);
          break;
        case "diff":
          if (message.data?.quotes) handlers.onQuote?.(message.data.quotes);
          if (message.data?.klines) {
            for (const [symbol, byDuration] of Object.entries(message.data.klines)) {
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
        const delay = Math.min(5000, 300 * 2 ** retry++);
        reconnectTimer = window.setTimeout(connect, delay);
      }
    };
    socket.onerror = () => {
      socket?.close();
    };
  };

  connect();

  return {
    subscribe(nextSymbols, nextKlines = []) {
      let changed = false;
      for (const symbol of nextSymbols) {
        if (!symbol) continue;
        if (!symbols.has(symbol)) {
          symbols.add(symbol);
          changed = true;
        }
      }
      for (const kline of nextKlines) {
        const key = `${kline.symbol}|${kline.duration_ns}`;
        if (!klines.has(key)) {
          klines.set(key, kline);
          changed = true;
        }
      }
      if (changed && online) sendSubscribe();
    },
    close() {
      closed = true;
      window.clearTimeout(reconnectTimer);
      window.clearInterval(pingTimer);
      socket?.close();
    },
  };
}
