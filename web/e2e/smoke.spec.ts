import { expect, test } from "@playwright/test";

test("symbol search preserves SHFE contract casing and shows safe custom indicators", async ({
  page,
}) => {
  const pageErrors: string[] = [];
  page.on("pageerror", (error) => pageErrors.push(error.message));

  await page.goto("/?theme=dark");
  await expect(page.locator(".symbol-title h1")).toHaveText("SHFE.rb2701");
  await expect(page.locator(".account-select select")).toBeVisible();
  await expect(page.locator(".chart-wasm")).toHaveText(["EMA6", "EMA12", "EMA24", "EMA60"]);

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
  await expect(settings.getByText("WASM 插件", { exact: true })).toBeVisible();
  await expect(settings.getByText("EMA 多均线 v0.2.0")).toBeVisible();
  const wasmPeriod = settings.locator(".wasm-indicator-block input[type=number]").first();
  await expect(wasmPeriod).toHaveValue("6");
  await wasmPeriod.fill("8");
  await wasmPeriod.blur();
  await expect(page.locator(".chart-wasm")).toHaveText(["EMA8", "EMA12", "EMA24", "EMA60"]);
  await expect(settings.getByText("内置参数指标", { exact: true })).toBeVisible();
  await expect(settings.getByText("兼容旧配置：仅 EMA/SMA 参数化计算；WASM 插件见上方。")).toBeVisible();

  expect(pageErrors).toEqual([]);
});
