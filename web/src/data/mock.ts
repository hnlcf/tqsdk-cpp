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

export interface FuturesRow {
  symbol: string;
  name: string;
  exchange: string;
  price: number;
  changePct: number;
  volume: number;
  openInterest: number;
  marginRate: number;
}

export interface OptionRow {
  strike: number;
  callLast: number;
  callChangePct: number;
  callVolume: number;
  putLast: number;
  putChangePct: number;
  putVolume: number;
}

export const FUTURES_ROWS: FuturesRow[] = [
  { symbol: "SHFE.rb2701", name: "螺纹钢 2701", exchange: "SHFE", price: 2993, changePct: 0.42, volume: 1_284_500, openInterest: 1_882_400, marginRate: 0.12 },
  { symbol: "SHFE.rb2705", name: "螺纹钢 2705", exchange: "SHFE", price: 3012, changePct: 0.18, volume: 842_300, openInterest: 1_150_200, marginRate: 0.12 },
  { symbol: "SHFE.au2702", name: "沪金 2702", exchange: "SHFE", price: 612.8, changePct: 0.76, volume: 123_400, openInterest: 212_900, marginRate: 0.10 },
  { symbol: "SHFE.cu2701", name: "沪铜 2701", exchange: "SHFE", price: 78420, changePct: -0.31, volume: 88_200, openInterest: 143_600, marginRate: 0.12 },
  { symbol: "DCE.m2701", name: "豆粕 2701", exchange: "DCE", price: 3126, changePct: 0.27, volume: 746_800, openInterest: 1_620_300, marginRate: 0.10 },
  { symbol: "DCE.i2701", name: "铁矿石 2701", exchange: "DCE", price: 782.5, changePct: -0.62, volume: 512_900, openInterest: 782_100, marginRate: 0.12 },
  { symbol: "DCE.jm2701", name: "焦煤 2701", exchange: "DCE", price: 1184, changePct: 1.24, volume: 288_600, openInterest: 402_700, marginRate: 0.14 },
  { symbol: "CZCE.SA701", name: "纯碱 701", exchange: "CZCE", price: 1418, changePct: -0.63, volume: 988_200, openInterest: 1_420_800, marginRate: 0.12 },
  { symbol: "CZCE.MA701", name: "甲醇 701", exchange: "CZCE", price: 2462, changePct: 0.39, volume: 624_100, openInterest: 884_600, marginRate: 0.10 },
  { symbol: "CZCE.TA701", name: "PTA 701", exchange: "CZCE", price: 4826, changePct: -0.18, volume: 382_700, openInterest: 612_300, marginRate: 0.10 },
  { symbol: "CFFEX.IF2611", name: "沪深300 2611", exchange: "CFFEX", price: 3984.6, changePct: 0.52, volume: 96_400, openInterest: 132_800, marginRate: 0.12 },
  { symbol: "INE.sc2612", name: "原油 2612", exchange: "INE", price: 548.7, changePct: 1.08, volume: 74_300, openInterest: 98_400, marginRate: 0.15 },
];

export const OPTION_UNDERLYING = "SHFE.rb2701";
export const OPTION_EXPIRY = "2026-12-15";
export const OPTION_ROWS: OptionRow[] = [
  { strike: 2900, callLast: 118.5, callChangePct: 8.2, callVolume: 1240, putLast: 12.5, putChangePct: -18.4, putVolume: 864 },
  { strike: 2920, callLast: 101.2, callChangePct: 7.4, callVolume: 986, putLast: 17.8, putChangePct: -16.2, putVolume: 742 },
  { strike: 2940, callLast: 85.4, callChangePct: 6.8, callVolume: 1510, putLast: 25.6, putChangePct: -13.5, putVolume: 1020 },
  { strike: 2960, callLast: 71.8, callChangePct: 5.9, callVolume: 1890, putLast: 36.2, putChangePct: -11.2, putVolume: 1320 },
  { strike: 2980, callLast: 59.6, callChangePct: 5.1, callVolume: 2260, putLast: 48.4, putChangePct: -8.6, putVolume: 1560 },
  { strike: 3000, callLast: 48.2, callChangePct: 4.2, callVolume: 2820, putLast: 63.5, putChangePct: -6.4, putVolume: 1880 },
  { strike: 3020, callLast: 38.5, callChangePct: 3.1, callVolume: 2100, putLast: 79.8, putChangePct: -4.8, putVolume: 1420 },
  { strike: 3040, callLast: 30.1, callChangePct: 2.2, callVolume: 1260, putLast: 98.2, putChangePct: -3.2, putVolume: 960 },
];
