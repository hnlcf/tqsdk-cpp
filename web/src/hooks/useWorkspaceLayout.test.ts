import { describe, expect, it } from "vitest";
import { applyResize } from "./useWorkspaceLayout";
import { DEFAULT_WORKSPACE_LAYOUT } from "../lib/storage";

describe("applyResize", () => {
  it("moves the left and right rails in the expected direction", () => {
    expect(applyResize("watchlist", 40, 0, DEFAULT_WORKSPACE_LAYOUT).watchlistWidth).toBe(276);
    expect(applyResize("rightRail", 40, 0, DEFAULT_WORKSPACE_LAYOUT).rightRailWidth).toBe(254);
    expect(applyResize("table", 0, 40, DEFAULT_WORKSPACE_LAYOUT).tableHeight).toBe(300);
  });

  it("clamps all panels to their usable bounds", () => {
    expect(applyResize("watchlist", -1000, 0, DEFAULT_WORKSPACE_LAYOUT).watchlistWidth).toBe(180);
    expect(applyResize("watchlist", 1000, 0, DEFAULT_WORKSPACE_LAYOUT).watchlistWidth).toBe(420);
    expect(applyResize("rightRail", -1000, 0, DEFAULT_WORKSPACE_LAYOUT).rightRailWidth).toBe(520);
    expect(applyResize("rightRail", 1000, 0, DEFAULT_WORKSPACE_LAYOUT).rightRailWidth).toBe(220);
    expect(applyResize("table", 0, -1000, DEFAULT_WORKSPACE_LAYOUT).tableHeight).toBe(120);
    expect(applyResize("table", 0, 1000, DEFAULT_WORKSPACE_LAYOUT).tableHeight).toBe(520);
  });
});
