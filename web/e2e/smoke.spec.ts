import { expect, test } from "@playwright/test";

test("symbol search preserves SHFE contract casing and shows safe custom indicators", async ({
  page,
}) => {
  const pageErrors: string[] = [];
  page.on("pageerror", (error) => pageErrors.push(error.message));

  await page.goto("/?theme=dark");
  await expect(page.locator(".symbol-title h1")).toHaveText("SHFE.rb2701");
  await expect(page.locator(".account-select select")).toBeVisible();
  await expect(page.locator(".chart-wasm")).toHaveText([
    "EMA5",
    "EMA10",
    "EMA20",
    "EMA60",
    "EMA120",
    "EMA200",
    "MA5",
    "MA10",
    "MA20",
    "MA60",
    "ATR14",
    "DC20",
    "DC20 MID",
  ]);

  await page.getByTitle("添加合约").click();
  const search = page.locator('.watch-add input[placeholder^="代码/名称"]');
  await expect(search).toBeVisible();
  await search.fill("ao");

  const suggestion = page.locator(".watch-suggestion", { hasText: "SHFE.ao2701" }).first();
  await expect(suggestion).toBeVisible();
  await suggestion.click();

  await expect(page.locator(".symbol-title h1")).toHaveText("SHFE.ao2701");
  await expect(page.locator(".watch-row", { hasText: "SHFE.ao2701" })).toBeVisible();
  await expect(page.locator(".quick-orders.four-key button")).toHaveCount(4);
  await expect(page.getByRole("button", { name: /平空/ })).toBeVisible();

  await page.locator(".indicator-settings-trigger").click();
  const settings = page.locator(".indicator-settings");
  await expect(settings).toBeVisible();
  await expect(settings.getByText("EMA 多均线", { exact: true })).toBeVisible();
  await expect(settings.getByText("MA 多均线", { exact: true })).toBeVisible();
  await expect(settings.getByText("ATR", { exact: true })).toBeVisible();
  await expect(settings.getByText("Donchian Channel", { exact: true })).toBeVisible();
  await expect(settings.getByText("BOLL", { exact: true })).toBeVisible();
  await expect(settings.getByText("MACD", { exact: true })).toBeVisible();
  await expect(settings.getByText("RSI", { exact: true })).toBeVisible();
  const wasmPeriod = settings.locator(".wasm-indicator-block input[type=number]").first();
  await expect(wasmPeriod).toHaveValue("5");
  await wasmPeriod.fill("8");
  await wasmPeriod.blur();
  await expect(page.locator(".chart-wasm").first()).toHaveText("EMA8");

  const macdBlock = settings.locator(".wasm-indicator-block", { hasText: "MACD" }).first();
  await macdBlock.locator("input[type=checkbox]").check();
  await settings.locator(".submit-order").click();
  await expect(page.locator(".chart-wasm", { hasText: "MACD DIF" })).toBeVisible();
  await expect(page.locator(".chart-wasm", { hasText: "MACD HIST" })).toBeVisible();

  expect(pageErrors).toEqual([]);
});
