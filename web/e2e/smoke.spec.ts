import { expect, test } from "@playwright/test";

test("symbol search preserves SHFE contract casing and shows safe custom indicators", async ({
  page,
}) => {
  const pageErrors: string[] = [];
  page.on("pageerror", (error) => pageErrors.push(error.message));

  await page.goto("/?theme=dark");
  await expect(page.locator(".symbol-title h1")).toHaveText("SHFE.rb2701");
  await expect(page.locator(".account-select select")).toBeVisible();

  await page.getByTitle("添加合约").click();
  const search = page.locator('.watch-add input[placeholder^="代码/名称"]');
  await expect(search).toBeVisible();
  await search.fill("ao");

  const suggestion = page.locator(".watch-suggestion", { hasText: "SHFE.ao2701" }).first();
  await expect(suggestion).toBeVisible();
  await suggestion.click();

  await expect(page.locator(".symbol-title h1")).toHaveText("SHFE.ao2701");
  await expect(page.locator(".watch-row", { hasText: "SHFE.ao2701" })).toBeVisible();

  await page.locator(".indicator-settings-trigger").click();
  const settings = page.locator(".indicator-settings");
  await expect(settings).toBeVisible();
  await expect(settings.getByText("自定义指标")).toBeVisible();
  await expect(settings.getByText("安全 DSL：只支持 EMA/SMA 白名单计算，不执行任意 JavaScript。")).toBeVisible();
  const customPeriod = settings.locator(".custom-indicator-row input[type=number]").first();
  await expect(customPeriod).toHaveValue("6");

  expect(pageErrors).toEqual([]);
});
