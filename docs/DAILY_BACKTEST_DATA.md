# 十年日线回测：数据、方法与产物

## 1. 数据源

正式回测使用 **Tq 官方 Backtest MD**：

```text
wss://backtest.shinnytech.com/t/nfmd/front/mobile
```

品种为对应商品的**指数连续**：

```text
KQ.i@SHFE.rb
KQ.i@DCE.jm
KQ.i@CZCE.SA
...
```

每个品种返回约 2,635 根日线，覆盖：

```text
2016-01-05 ~ 2026-11-11
```

这比实时免费行情的 200 根窗口长得多，因此可以真正做十年回测。

## 2. 凭据与安全

下载器从以下来源读取凭据，**不会把账号密码写进 Git 或报告**：

- 环境变量 `TQSDK_USERNAME` / `TQSDK_PASSWORD`；
- 或 `TQ_AUTH_USER` / `TQ_AUTH_PASSWORD`；
- 或 `--env-file` 指定的本地 env 文件（默认 `config/live.local.env`）。

`config/live.local.env` 权限应保持 `0600`，不要提交。

## 3. 下载

```bash
python3 scripts/fetch_tq_daily.py \
  --env-file config/live.local.env \
  --output-dir artifacts/tq_daily
```

产物：

```text
artifacts/tq_daily/KQ.i@SHFE.rb.json
artifacts/tq_daily/KQ.i@DCE.jm.json
...
artifacts/tq_daily/_failures.json
```

> 实时免费 MD `free-openmd.shinnytech.com` 曾出现 443 连接拒绝；官方
> backtest 端点仍可用，且本身就是历史研究用途。生产实时行情不应依赖
> backtest 端点。

## 4. 股票池过滤

从 `continuous_table.json` 得到 89 个商品：

- 远端无数据：`CZCE.SO` 等，排除；
- 最近 250 根日线成交量中位数 `< 5000`：排除；
- 最近成交量零成交占比过高：排除；
- 少于 500 根日线（上市过晚）：排除。

最终纳入 **69 个品种**。完整排除清单见正式报告第 1.1 节。

过滤后仍覆盖：

- SHFE：rb、ru、au、ag、cu、al、zn、ni、sn、pb、ss、hc、sp、bu、fu、ao、br；
- DCE：m、y、p、c、i、j、jm、v、pp、l、eg、eb、pg、lh、jd、a、b、cs；
- CZCE：SA、MA、TA、SR、CF、CY、FG、AP、CJ、PK、UR、OI、RM、SM、SH、PX、PR 等；
- INE：sc、lu、nr、bc、ec；
- GFEX：si、lc；
- CFFEX：IF、IH、IC、IM、T、TF、TS、TL。

## 5. 回测规则

- 周期：日线；
- 指标：EMA60、Donchian22（前 22 根，不含当前）、ATR14；
- 多头：`close > EMA60` 且 `close > Donchian upper`；
- 空头：`close < EMA60` 且 `close < Donchian lower`；
- 入场：信号收盘产生，下一交易日开盘成交；
- 止损：入场价 ∓ 2×ATR；
- 退出：收盘跌破通道/EMA，下一交易日开盘成交；
- 仓位：每品种 30% 名义仓位；
- 手续费：单边 1 bp；
- 每品种最多一个方向、无金字塔加仓、不进行同日反手。

## 6. 报告与产物

正式报告：

```text
docs/DAILY_STRATEGY_BACKTEST.md
```

图表：

```text
docs/backtest_charts/portfolio_equity.svg
docs/backtest_charts/portfolio_drawdown.svg
docs/backtest_charts/yearly_portfolio.svg
docs/backtest_charts/return_ranking.svg
docs/backtest_charts/risk_return_scatter.svg
docs/backtest_charts/yearly_heatmap.svg
docs/backtest_charts/top_equity.svg
docs/backtest_charts/trade_histogram.svg
docs/backtest_charts/long_short.svg
```

机器可读数据：

```text
artifacts/daily_backtest/results.csv
artifacts/daily_backtest/results.json
artifacts/daily_backtest/trades.csv
artifacts/daily_backtest/yearly_returns.csv
```

## 7. 复现

```bash
python3 scripts/fetch_tq_daily.py \
  --env-file config/live.local.env \
  --output-dir artifacts/tq_daily

python3 scripts/backtest_daily.py \
  --data-dir artifacts/tq_daily \
  --report docs/DAILY_STRATEGY_BACKTEST.md \
  --charts-dir docs/backtest_charts \
  --output-dir artifacts/daily_backtest
```

## 8. 解释边界

- 组合曲线是 69 个品种各自 30% 名义策略日收益的等权平均，不是单一账户逐笔撮合；
- 品种池按当前 `continuous_table` 与当前流动性筛选，存在幸存者偏差；
- 指数连续没有换月跳空，适合研究信号，不等于可直接交易的主连；
- 未建模滑点、涨跌停、保证金、合约乘数差异和夜盘细节；
- 新上市品种只从上市日开始交易，首年指标是部分年份。
