export interface Bar {
  time: number;
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
}

export interface Quote {
  symbol: string;
  name: string;
  price: number;
  change: number;
  changePct: number;
  bid: number;
  ask: number;
  volume: number;
  openInterest: number;
}

export interface Position {
  symbol: string;
  side: "LONG" | "SHORT";
  volume: number;
  openPrice: number;
  lastPrice: number;
  floatProfit: number;
  margin: number;
}

export interface OrderRow {
  id: string;
  symbol: string;
  side: "BUY" | "SELL";
  offset: "OPEN" | "CLOSE";
  price: number;
  volume: number;
  status: "ALIVE" | "FINISHED";
  message: string;
}

export interface SymbolMeta {
  symbol: string;
  name: string;
  price: number;
  changePct: number;
}

export const WATCHLIST: SymbolMeta[] = [
  { symbol: "SHFE.rb2701", name: "螺纹钢 2701", price: 2993, changePct: 0.42 },
  { symbol: "SHFE.rb2705", name: "螺纹钢 2705", price: 3012, changePct: 0.18 },
  { symbol: "CZCE.SA701", name: "纯碱 701", price: 1418, changePct: -0.63 },
  { symbol: "DCE.m2701", name: "豆粕 2701", price: 3126, changePct: 0.27 },
];

function mulberry32(seed: number) {
  return () => {
    let t = (seed += 0x6d2b79f5);
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export function createMockBars(count = 260, base = 2968, seed = 20261103): Bar[] {
  const random = mulberry32(seed);
  const now = Math.floor(Date.now() / 1000 / 60) * 60;
  const bars: Bar[] = [];
  let close = base;

  for (let i = count - 1; i >= 0; i--) {
    const open = close;
    const drift = (random() - 0.47) * 14;
    close = Math.max(2500, open + drift);
    const high = Math.max(open, close) + random() * 5;
    const low = Math.min(open, close) - random() * 5;
    bars.push({
      time: now - i * 60,
      open: round2(open),
      high: round2(high),
      low: round2(low),
      close: round2(close),
      volume: Math.round(900 + random() * 3600),
    });
  }
  return bars;
}

export function nextTick(previous: Bar, random = Math.random): Bar {
  const delta = (random() - 0.5) * 7;
  const close = round2(Math.max(2500, previous.close + delta));
  return {
    ...previous,
    high: round2(Math.max(previous.high, close)),
    low: round2(Math.min(previous.low, close)),
    close,
    volume: previous.volume + Math.round(40 + random() * 260),
  };
}

export function quoteFromBars(symbol: string, name: string, bars: Bar[]): Quote {
  const last = bars[bars.length - 1];
  const previous = bars[bars.length - 2] ?? last;
  const change = round2(last.close - previous.close);
  return {
    symbol,
    name,
    price: last.close,
    change,
    changePct: round2((change / previous.close) * 100),
    bid: round2(last.close - 1),
    ask: round2(last.close + 1),
    volume: bars.slice(-20).reduce((sum, bar) => sum + bar.volume, 0),
    openInterest: 188_240,
  };
}

export const MOCK_POSITIONS: Position[] = [
  {
    symbol: "SHFE.rb2701",
    side: "LONG",
    volume: 2,
    openPrice: 2968,
    lastPrice: 2993,
    floatProfit: 500,
    margin: 17_958,
  },
  {
    symbol: "CZCE.SA701",
    side: "SHORT",
    volume: 1,
    openPrice: 1426,
    lastPrice: 1418,
    floatProfit: 160,
    margin: 3_420,
  },
];

export const MOCK_ORDERS: OrderRow[] = [
  {
    id: "r1793688008",
    symbol: "SHFE.rb2701",
    side: "BUY",
    offset: "OPEN",
    price: 2980,
    volume: 1,
    status: "ALIVE",
    message: "未成交",
  },
  {
    id: "r1793687842",
    symbol: "SHFE.rb2701",
    side: "BUY",
    offset: "OPEN",
    price: 2987,
    volume: 1,
    status: "FINISHED",
    message: "已撤单",
  },
];

function round2(value: number): number {
  return Math.round(value * 100) / 100;
}
