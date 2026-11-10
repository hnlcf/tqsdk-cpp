# CTP 交易矩阵与 5 交易日执行清单

本文件把已有脚本组织成可执行清单。真实下单/平仓必须使用专用测试账户，并显式确认；
任何一步失败都应停止，不要在下一笔订单中“带病继续”。

## 1. 前置

```bash
./scripts/ctp_week_preflight.sh
python3 scripts/ctp_week_soak.py --days 5          # dry-run
```

确认：

- gw/node 使用真实的 CTP 测试/SimNow 账户；
- 合约有有效行情（任何/BEST 单必须有 quote）；
- `jq`、`curl`、Python 依赖可用；
- 记录账户、合约、交易日和脚本版本。

## 2. 连续 5 交易日

```bash
python3 scripts/ctp_week_soak.py --days 5 --execute
```

每个交易日开盘前/收盘后：

```bash
python3 scripts/ctp_daily_reconcile.py \
  --node-url http://127.0.0.1:18081 \
  --out-dir artifacts/ctp_reconcile
```

通过条件见 [CTP_WEEKLY_TEST_PLAN.md](CTP_WEEKLY_TEST_PLAN.md)：

- 非预期 `unexpected_gaps=0`；
- 结算断线按 `expected_disconnect` 分类；
- 每日持仓/资金/订单对账一致；
- gw/node 无未解释退出。

## 3. 平昨验收

先在前一交易日建立可结算历史仓：

```bash
NODE_URL=http://127.0.0.1:18081 \
SYMBOL=SHFE.rb2701 \
./scripts/ctp_open_overnight_position.sh
```

下一交易日结算后：

```bash
NODE_URL=http://127.0.0.1:18081 \
SYMBOL=SHFE.rb2701 \
./scripts/ctp_close_yesterday_test.sh
```

## 4. 交易所矩阵

建议至少覆盖：

| 交易所 | 品种示例 | 平今 | 平昨 | 备注 |
|---|---|---|---|---|
| SHFE | rb/ao | `CLOSETODAY` | `CLOSEYESTERDAY` | 市价/对价需 quote |
| INE | sc | `CLOSETODAY` | `CLOSEYESTERDAY` | 夜盘边界 |
| GFEX | si/lc | `CLOSETODAY` | `CLOSEYESTERDAY` | 以柜台规则为准 |
| CZCE | SA/MA | 通用 `CLOSE` | 通用 `CLOSE` | 合约代码大写 |
| DCE | m/i/jm | 通用 `CLOSE` | 通用 `CLOSE` | 合约代码小写 |

每项检查：

1. `/order` 返回 order_id；
2. 订单最终 `is_dead=true` 且 `is_error=false`；
3. `/positions` 的目标方向/今昨仓变化符合预期；
4. `/trades` 有对应成交；
5. CTP 柜台页面/对账文件一致。

## 5. 失败边界

以下不能当作 PASS：

- 无行情时 ANY/BEST 被拒绝：应记录为“行情缺失预期拒绝”，而不是成交；
- 平昨仓不足：应保留错误订单并核对 `CLOSED_YESTERDAY`/`CLOSE`；
- 结算窗口断线：应标记 `expected_disconnect`；
- 部分成交：必须等待终态或明确撤单结果，不能只看提交 ack。

## 6. 当前状态

交易矩阵脚本/手工步骤已准备；连续 5 个交易日和真实 CTP 账户执行尚未在当前环境完成。
执行结果应写入 `artifacts/ctp_week/` 和 `artifacts/ctp_reconcile/`，不要只提交“进程存活”
作为验收结论。
