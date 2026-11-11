import { expect, test } from "@playwright/test";

test("indicator picker, multi-EMA editing, output visibility and symbol casing", async ({
  page,
}) => {
  const pageErrors: string[] = [];
  page.on("pageerror", (error) => pageErrors.push(error.message));

  await page.goto("/?theme=dark");
  await expect(page.locator(".symbol-title h1")).toHaveText("SHFE.rb2701");
  await expect(page.locator(".account-select select")).toBeVisible();
  const watchlist = page.locator(".watchlist");
  const beforeResize = await watchlist.boundingBox();
  const leftSplitter = page.locator(".panel-resizer.vertical").first();
  const splitterBox = await leftSplitter.boundingBox();
  if (!beforeResize || !splitterBox) throw new Error("layout boxes missing");
  await page.mouse.move(splitterBox.x + 2, splitterBox.y + 60);
  await page.mouse.down();
  await page.mouse.move(splitterBox.x + 62, splitterBox.y + 60, { steps: 5 });
  await page.mouse.up();
  await expect
    .poll(async () => Math.round((await watchlist.boundingBox())?.width ?? 0))
    .toBeGreaterThan(Math.round(beforeResize.width) + 30);
  await expect(page.locator(".chart-wasm")).toHaveText([
    "EMA5",
    "EMA10",
    "EMA20",
    "EMA60",
    "EMA120",
    "EMA200",
    "MACD DIF",
    "MACD DEA",
    "MACD HIST",
  ]);
  await expect(page.locator(".account-select select")).toHaveValue("default");
  const fiveMinute = page.getByRole("button", { name: "5m", exact: true });
  await fiveMinute.click();
  await expect(fiveMinute).toHaveClass(/active/);
  const canvas = page.locator(".chart-canvas");
  await expect(canvas).toBeVisible();
  const canvasBox = await canvas.boundingBox();
  if (!canvasBox) throw new Error("chart canvas box missing");
  await page.mouse.move(canvasBox.x + canvasBox.width / 2, canvasBox.y + canvasBox.height / 2);
  await page.mouse.wheel(0, -240);
  await page.getByRole("button", { name: "分时", exact: true }).click();
  await expect(page.getByRole("button", { name: "分时", exact: true })).toHaveClass(/active/);
  await page.getByRole("button", { name: "1m", exact: true }).click();
  await expect(page.getByRole("button", { name: "1m", exact: true })).toHaveClass(/active/);
  await page.getByRole("button", { name: "清除画线" }).click();
  await page.getByRole("button", { name: "画趋势线" }).click();
  await page.locator(".chart-canvas").click({ position: { x: canvasBox.width * 0.35, y: canvasBox.height * 0.4 } });
  await page.waitForTimeout(700);
  await page.locator(".chart-canvas").click({ position: { x: canvasBox.width * 0.65, y: canvasBox.height * 0.6 } });
  await expect(page.locator(".drawing-count")).toHaveText("画线 1");
  await page.getByRole("button", { name: "清除画线" }).click();
  await expect(page.locator(".drawing-count")).toHaveText("画线 0");
  await page.getByRole("button", { name: "画水平线" }).click();
  await page.waitForTimeout(700);
  await page.locator(".chart-canvas").click({ position: { x: canvasBox.width / 2, y: canvasBox.height / 2 } });
  await expect(page.locator(".drawing-count")).toHaveText("画线 1");
  await page.reload();
  await expect(page.locator(".drawing-count")).toHaveText("画线 1");
  await page.getByRole("button", { name: "清除画线" }).click();
  await expect(page.locator(".drawing-count")).toHaveText("画线 0");

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
  const popover = page.locator(".indicator-popover");
  await expect(popover).toBeVisible();
  await expect(popover.locator(".indicator-picker-row")).toHaveCount(11);
  for (const name of [/^EMA 多均线/, /^MA 多均线/, /^BOLL/, /^MACD/, /^RSI/, /^ATR/, /^Donchian Channel/, /^KDJ/, /^CCI/, /^OBV/, /^VWAP/]) {
    await expect(popover.locator(".indicator-picker-row", { hasText: name }).first()).toBeVisible();
  }

  await popover.locator(".indicator-picker-row", { hasText: /^EMA 多均线/ }).first().locator(".indicator-picker-open").click();
  await expect(popover.locator(".indicator-line-config")).toHaveCount(6);
  const firstPeriod = popover
    .locator(".indicator-line-config .indicator-param-row input[type=number]")
    .first();
  await expect(firstPeriod).toHaveValue("5");
  await firstPeriod.fill("8");
  await firstPeriod.blur();
  await expect(popover.locator(".indicator-line-config select").first()).toBeVisible();
  await expect(popover.locator(".indicator-line-config input[type=color]").first()).toBeVisible();
  await popover.locator(".indicator-line-config select").first().selectOption("dashed");

  await popover.locator(".indicator-add-line").click();
  await expect(popover.locator(".indicator-line-config")).toHaveCount(7);
  await popover.locator(".indicator-line-config .table-action.danger").last().click();
  await expect(popover.locator(".indicator-line-config")).toHaveCount(6);
  await popover.locator(".indicator-back").click();
  const maRow = popover.locator(".indicator-picker-row", { hasText: /^MA 多均线/ }).first();
  const maToggle = maRow.locator(".indicator-picker-toggle input");
  await maToggle.check();
  await expect(maToggle).toBeChecked();
  await expect(maRow).toHaveClass(/active/);
  await popover.locator('.indicator-popover-head .icon-button[title="关闭"]').click();
  await expect(page.locator(".chart-wasm").first()).toHaveText("EMA8");
  await expect(page.locator(".chart-wasm", { hasText: "MA5" })).toBeVisible();

  await page.locator(".indicator-settings-trigger").click();
  const popover2 = page.locator(".indicator-popover");
  await popover2.locator(".indicator-picker-row", { hasText: /^MACD/ }).first().locator(".indicator-picker-open").click();
  const difRow = popover2.locator(".output-config-row").first();
  await difRow.locator('input[type="checkbox"]').uncheck();
  await popover2.locator('.indicator-popover-head .icon-button[title="关闭"]').click();
  await expect(page.locator(".chart-wasm", { hasText: "MACD DIF" })).toHaveCount(0);
  await expect(page.locator(".chart-wasm", { hasText: "MACD HIST" })).toBeVisible();

  await page.reload();
  await expect
    .poll(async () => Math.round((await page.locator(".watchlist").boundingBox())?.width ?? 0))
    .toBeGreaterThan(Math.round(beforeResize.width) + 30);
  await expect(page.locator(".chart-wasm").first()).toHaveText("EMA8");
  await expect(page.locator(".chart-wasm", { hasText: "MA5" })).toBeVisible();
  await expect(page.locator(".chart-wasm", { hasText: "MACD DIF" })).toHaveCount(0);

  await page.locator(".indicator-settings-trigger").click();
  const restored = page.locator(".indicator-popover");
  await restored.locator(".indicator-picker-row", { hasText: /^EMA 多均线/ }).first().locator(".indicator-picker-open").click();
  await expect(
    restored.locator(".indicator-line-config .indicator-param-row input[type=number]").first(),
  ).toHaveValue("8");
  await expect(restored.locator(".indicator-line-config select").first()).toHaveValue("dashed");

  expect(pageErrors).toEqual([]);
});
