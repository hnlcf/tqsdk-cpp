# 2026-11-11 策略扫描记录

- 数据源：Tq MD `free-openmd.shinnytech.com`
- 周期：4h，`KQ.i@<product>` 指数连续
- 扫描品种：54 个（SHFE/DCE/CZCE/INE/GFEX 主流品种）
- 每品种可用 bars：200
- 参数：EMA60、Donchian22、ATR14、2ATR 止损、30% 名义仓位、单边手续费 0.0001
- 数据时间范围：约 2026-01 至 2026-11（具体见 JSON 的 `bars_from/bars_to`）

## 结果摘要

| 品种 | 交易数 | 胜率 | 总收益 | 最大回撤 |
|---|---:|---:|---:|---:|
| GFEX.lc | 2 | 100% | +11.43% | 0.00% |
| INE.lu | 2 | 100% | +6.36% | 0.00% |
| SHFE.fu | 3 | 66.7% | +5.78% | 0.32% |
| CZCE.AP | 2 | 50% | +5.66% | 0.31% |
| CZCE.CJ | 2 | 50% | +4.66% | 0.78% |

完整结果：

```text
artifacts/strategy_scan/strategy_scan_*.json
artifacts/strategy_scan/strategy_scan_*.csv
```

## 结论边界

这不是长周期统计。Tq MD 该接口当前对每个合约返回约 200 根 4h bar，等价几十个
交易日；交易样本只有 1–5 笔，收益和胜率不具备统计显著性。要验证策略长期
有效性，需要扩展历史 4h 数据源（或先导入本地 K 线缓存），再做至少数年、
跨多个换月周期的回测。

本次运行没有向 CTP 下单。真实 CTP 策略执行需要在可交易时段、使用实际合约，
并且只使用已确认的 SimNow 账户权限。
