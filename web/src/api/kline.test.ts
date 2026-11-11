import { afterEach, describe, expect, it, vi } from "vitest";
import { getKline } from "./client";

describe("getKline adjustment query", () => {
  afterEach(() => vi.unstubAllGlobals());

  it("adds adjust for continuous daily requests", async () => {
    let called = "";
    vi.stubGlobal("fetch", async (input: string | URL | Request) => {
      called = String(input);
      return new Response(JSON.stringify({ symbol: "KQ.m@DCE.jm", duration_ns: 86_400_000_000_000, bars: [] }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      });
    });
    await getKline("KQ.m@DCE.jm", 86_400_000_000_000, "back");
    expect(called).toContain("/kline/KQ.m%40DCE.jm/86400000000000?adjust=back");
  });

  it("does not add a query for raw data", async () => {
    let called = "";
    vi.stubGlobal("fetch", async (input: string | URL | Request) => {
      called = String(input);
      return new Response(JSON.stringify({ symbol: "X", duration_ns: 1, bars: [] }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      });
    });
    await getKline("X", 1);
    expect(called).not.toContain("adjust=");
  });
});
