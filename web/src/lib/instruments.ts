const PRODUCT_NAMES: Record<string, string> = {
  rb: "螺纹钢",
  ru: "天然橡胶",
  m: "豆粕",
  y: "豆油",
  jm: "焦煤",
  i: "铁矿石",
  SA: "纯碱",
  TA: "PTA",
  MA: "甲醇",
  au: "沪金",
  ag: "沪银",
  cu: "沪铜",
  al: "沪铝",
  IF: "沪深300",
  IC: "中证500",
  IH: "上证50",
  IM: "中证1000",
  sc: "原油",
  ao: "氧化铝",
};

const VOLUME_MULTIPLIERS: Record<string, number> = {
  rb: 10,
  ru: 10,
  m: 10,
  y: 10,
  jm: 60,
  i: 100,
  SA: 20,
  TA: 5,
  MA: 10,
  au: 1000,
  ag: 15,
  cu: 5,
  al: 5,
  IF: 300,
  IC: 200,
  IH: 300,
  IM: 200,
  sc: 1000,
  ao: 20,
};

export function productOf(symbol: string): string {
  const contract = symbol.includes(".") ? symbol.slice(symbol.indexOf(".") + 1) : symbol;
  return contract.replace(/\d+$/, "");
}

export function instrumentName(symbol: string): string {
  const product = productOf(symbol);
  const name = PRODUCT_NAMES[product];
  const contract = symbol.includes(".") ? symbol.slice(symbol.indexOf(".") + 1) : symbol;
  return name ? `${name} ${contract}` : contract;
}

export function fallbackVolumeMultiple(symbol: string): number {
  return VOLUME_MULTIPLIERS[productOf(symbol)] ?? 1;
}

export function exchangeOf(symbol: string): string {
  return symbol.includes(".") ? symbol.slice(0, symbol.indexOf(".")) : "";
}

export interface FallbackInstrument {
  symbol: string;
  name: string;
  exchange: string;
}

const FALLBACK_SYMBOLS = [
  "SHFE.rb2701",
  "SHFE.ru2701",
  "SHFE.au2701",
  "SHFE.cu2701",
  "DCE.m2701",
  "DCE.jm2701",
  "DCE.y2701",
  "DCE.i2701",
  "CZCE.SA701",
  "CZCE.TA701",
  "CZCE.MA701",
  "CFFEX.IF2611",
  "INE.sc2612",
];

/**
 * Catalog is best-effort: if node has not finished downloading symbol.json,
 * the add box must still be useful for the common contracts and product names.
 */
export function searchFallbackInstruments(query: string): FallbackInstrument[] {
  const value = query.trim().toLowerCase();
  if (!value) return [];
  return FALLBACK_SYMBOLS.map((symbol) => ({
    symbol,
    name: instrumentName(symbol),
    exchange: exchangeOf(symbol),
  }))
    .filter((item) => {
      const haystack = `${item.symbol} ${item.name} ${productOf(item.symbol)}`.toLowerCase();
      return haystack.includes(value);
    })
    .slice(0, 8);
}
