# SimNow / CTP 验收

本文是 CTP 真实账户验收入口。架构和协议分别见
[ARCHITECTURE.md](ARCHITECTURE.md)、[CTP.md](CTP.md)。

## 1. 启动环境

```bash
cp config/live.local.env.example config/live.local.env
chmod 600 config/live.local.env
$EDITOR config/live.local.env

./scripts/run_live.sh --no-ui
```

默认 node：`http://127.0.0.1:18081`。

## 2. 单账户只读检查

```bash
python3 scripts/simnow_acceptance.py \
  --node-url http://127.0.0.1:18081 \
  --symbol SHFE.rb2701 \
  --metrics
```

覆盖 `/healthz`、`/readyz`、账户、持仓、订单、成交、quote、五档盘口和
WebSocket 订阅。默认不会下单。

## 3. 限价/撤单/错误单

```bash
python3 scripts/simnow_acceptance.py \
  --symbol SHFE.rb2701 \
  --orders --yes
```

脚本会：

1. 读取 quote 和 `price_tick`；
2. 提交远离盘口的小额限价单；
3. 确认订单可见；
4. 撤单并等待终态；
5. 提交 `volume=0` 错误单验证拒绝链路。

只预览不提交：

```bash
python3 scripts/simnow_acceptance.py --orders --dry-run
```

`--orders` 必须同时带 `--yes`，否则拒绝执行。

## 4. 市价/对价

市价和对价可能成交并产生持仓，默认关闭：

```bash
python3 scripts/simnow_acceptance.py \
  --symbol SHFE.rb2701 \
  --orders --yes \
  --include-market \
  --include-counterparty \
  --cleanup-market
```

SHFE 会在 node 侧转换为合法限价单：

- 市价 BUY -> 涨停价，SELL -> 跌停价；
- 对价 BUY -> 卖一价，SELL -> 买一价；
- 没有有效 quote/停板价时直接拒绝；
- `--cleanup-market` 只平掉测试期间新增的净持仓。

## 5. 报告和自测

```bash
python3 scripts/test_simnow_acceptance.py
```

报告默认写入：

```text
artifacts/simnow_acceptance_YYYYmmdd_HHMMSS.json
artifacts/simnow_acceptance_YYYYmmdd_HHMMSS.md
```

退出码：`0` 无 FAIL，`1` 有 FAIL，`2` 参数/安全确认失败。

## 6. 重启恢复

```bash
./scripts/ctp_recovery_test.sh
```

该脚本在隔离端口启动 gw/node，验证 gateway 重启、node 重启、`/readyz`
恢复以及 account/positions/orders 快照一致。建议阅读脚本顶部注释后使用
隔离配置：

```bash
NODE_URL=http://127.0.0.1:28081 \
GW_CONFIG=/tmp/ctp_recovery_gw.toml \
NODE_CONFIG=/tmp/ctp_recovery_node.toml \
./scripts/ctp_recovery_test.sh
```

## 7. 双账户

真实双账户流程见 [SIMNOW_DUAL_ACCOUNT.md](SIMNOW_DUAL_ACCOUNT.md)：

```bash
python3 scripts/simnow_dual_acceptance.py --orders --yes --symbol DCE.v2701
```

两个 Sim 网关的多账户路由验收见
[MULTI_ACCOUNT_ACCEPTANCE.md](MULTI_ACCOUNT_ACCEPTANCE.md)。

## 8. 连续交易时段

```bash
./scripts/ctp_week_preflight.sh
python3 scripts/ctp_week_soak.py --days 5          # dry-run
python3 scripts/ctp_week_soak.py --days 5 --execute
python3 scripts/ctp_daily_reconcile.py --out-dir artifacts/ctp_reconcile
```

CTP 账户在每日结算期间预期断线，不能把连续 24h 在线作为通过标准。正确口径：

```text
交易时段采样
  -> 收盘/结算前快照
  -> 预期断线
  -> 下一交易时段重连
  -> 对账 account/positions/orders/trades
```

## 9. 边界

- 验收脚本不启动 gw/node，复用 `run_live.sh` 或隔离配置；
- 真实下单必须显式 `--yes`，不要把脚本存在当成验收通过；
- 原始日志和 JSON/Markdown 报告写入 `artifacts/`，不提交 credentials；
- 部分成交受 SimNow 撮合和盘口深度影响，不作为稳定 PASS 条件；
- 当前真实 5 交易日结果以 `artifacts/` 和 [SOAK_PERF.md](SOAK_PERF.md) 为准。
