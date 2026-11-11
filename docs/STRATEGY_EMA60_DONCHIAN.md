# EMA60 + Donchian22 策略扫描

## 1. 规则

以 4h K 线为信号周期：

- 指标：
  - EMA60(close)；
  - Donchian 22（使用当前 bar 之前的 22 根 high/low）；
  - ATR14。
- 多头入场：`close > EMA60` 且 `close > Donchian upper`；
- 空头入场：`close < EMA60` 且 `close < Donchian lower`；
- 初始止损：入场价 ∓ `2 * ATR14`；
- 多头退出：收盘跌破 Donchian lower 或 EMA60；空头反之；
- 仓位：当前权益的 30% 作为名义仓位；
- 扫描使用的连续合约：`KQ.i@<product>` 指数连续，避免换月跳空污染 4h 信号；
- 手续费：单边 `0.0001`，用于回测近似。

## 2. 数据与执行

启动 Tq Md 数据服务：

```bash
./scripts/run_live.sh --no-ui
```

扫描：

```bash
python3 scripts/strategy_scan.py \
  --node-url http://127.0.0.1:18081 \
  --workers 6 \
  --output-dir artifacts/strategy_scan
```

当前默认扫描列表覆盖：

```text
SHFE / DCE / CZCE / INE / GFEX 的主要活跃品种，共 54 个品种
```

## 3. 正式十年日线回测

本文件记录的是早期 4h 扫描；正式回测改为日线、2016-2026、69 个品种，见 [DAILY_STRATEGY_BACKTEST.md](DAILY_STRATEGY_BACKTEST.md)。

## 4. 早期 4h 扫描实际结果

2026-11-11 运行结果保存于：

```text
artifacts/strategy_scan/strategy_scan_*.json
artifacts/strategy_scan/strategy_scan_*.csv
```

前几名（仅代表当前可获得的 200 根 4h 数据）：

| 品种 | 交易数 | 胜率 | 收益 |
|---|---:|---:|---:|
| GFEX.lc | 2 | 100% | +11.43% |
| INE.lu | 2 | 100% | +6.36% |
| SHFE.fu | 3 | 66.7% | +5.78% |
| CZCE.AP | 2 | 50% | +5.66% |
| CZCE.CJ | 2 | 50% | +4.66% |

**重要限制**：Tq MD 当前返回的 4h K 线约 200 根，等价约几十个交易日；
这不是多年统计，不能据此判断策略长期有效性。若要长周期回测，需要额外的
历史 4h 数据源，或先把历史 bar 导入本地缓存。

## 4. SimNow / CTP

策略扫描默认**不下单**。真实 CTP 执行需要：

- 一个真实 SimNow/CTP 账户；
- `config/live.local.env`，权限 0600；
- CTP 交易处于可交易时段（避开结算窗口）；
- 使用实际可交易合约，而不是 `KQ.i@` 指数。

当前只有一个 SimNow 账户，无法完成双账户资金/持仓隔离验收；单账户可用于：

- CTP 登录/行情/账户查询；
- 小仓位 LIMIT 单和撤单；
- TargetPosTask 执行链路；
- 最新 4h 信号的纸面/单笔执行验证。

不要把一个账户的测试结果写成“双账户通过”。

当前环境在 2026-11-11 晚间处于 CTP 结算预期断线阶段，实际下单仍需等到
下一个可交易时段。
