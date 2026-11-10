import { describe, expect, it } from "vitest";
import { mergeTradeData, type TradingState } from "./tradingData";

const empty: TradingState = { positions: {}, orders: {}, trades: {} };
const position = { instrument_id: "SHFE.rb2701", volume_long: 1 } as never;

describe("mergeTradeData", () => {
  it("snapshot replaces all maps", () => {
    const previous: TradingState = {
      ...empty,
      positions: { old: position },
      orders: { old: {} as never },
      trades: { old: {} as never },
    };
    const next = mergeTradeData(
      previous,
      { positions: { "SHFE.rb2701": position }, account: { balance: 1 } },
      true,
    );
    expect(next.positions).toEqual({ "SHFE.rb2701": position });
    expect(next.orders).toEqual({});
    expect(next.trades).toEqual({});
    expect(next.account?.balance).toBe(1);
  });

  it("diff merges by key and keeps omitted maps", () => {
    const previous: TradingState = {
      ...empty,
      positions: { old: position },
      orders: { old: {} as never },
    };
    const next = mergeTradeData(
      previous,
      { positions: { new: position }, orders: { fresh: {} as never } },
      false,
    );
    expect(Object.keys(next.positions).sort()).toEqual(["new", "old"]);
    expect(Object.keys(next.orders).sort()).toEqual(["fresh", "old"]);
    expect(next.trades).toEqual({});
  });
});
