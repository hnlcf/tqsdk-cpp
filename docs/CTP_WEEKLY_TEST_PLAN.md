# CTP 连续交易周验证计划

> 用途：P6.1 的真实交易日验证。本文命令应在下一个交易周执行，不能在周末
> 用 SimTd 或单个夜盘替代。

## 1. 目标

验证以下事实，而不是只看进程是否存活：

- CTP TD 在连续 5 个交易日的登录/结算/查询窗口可恢复；
- 结算断线被识别为 `expected_disconnect`，不计为故障；
- 盘中非预期断线、查询 watchdog、节点重启均可恢复；
- 账户/持仓/订单在每个交易日对账一致；
- 平今/平昨和开盘/收盘边界不产生非法报单。

> 自动排期由 `scripts/p6_schedule.sh` 完成，详细时间表见
> [P6_AUTOMATION.md](P6_AUTOMATION.md)。

## 2. 前置检查

在交易周开始前，先启动一次 node 或准备 live 配置，然后执行：

```bash
./scripts/ctp_week_preflight.sh
```

要求：

- `build-ui` 或 `build-ctp` 中的 gw/node 可执行文件存在；
- `config/live.local.env` 或环境变量包含 CTP 账号；
- 没有残留 gw/node；
- `jq` 可用；
- 如果 node 已启动，`/healthz` 可用。

## 3. 只读连续周 soak

脚本默认 **dry-run**；真正执行必须显式 `--execute`：

```bash
python3 scripts/ctp_week_soak.py --days 5
python3 scripts/ctp_week_soak.py --days 5 --execute
```

输出：

```text
artifacts/ctp_week/ctp_week_<tag>.jsonl
artifacts/ctp_week/ctp_week_<tag>.gw.log
artifacts/ctp_week/ctp_week_<tag>.node.log
artifacts/ctp_week/ctp_week_<tag>.summary.json
artifacts/ctp_week/ctp_week_<tag>.report.json
```

这个 soak **不下单**，只采样 `/readyz`、账户、持仓、订单数量。
`duration` 默认为 `days*86400` 秒；从交易周开始运行，跨过每日结算和周末。

完成标准：

- `report.monitor.unexpected_gaps == 0`；
- `report.monitor.fatal == ""`；
- 每个交易日有至少 1 个 `ready=true` 采样；
- 结算窗口可见 `phase=settlement` 或 `closed` 且 expected gap；
- gw/node 进程没有非预期退出。

> CTP 账户每日结算会断线是预期行为，不能要求 24h 连续 `ready=true`。

## 4. 每日对账

每天开盘前、收盘后各执行一次：

```bash
python3 scripts/ctp_daily_reconcile.py \
  --node-url http://127.0.0.1:18081 \
  --out-dir artifacts/ctp_reconcile
```

与上一交易日比较：

```bash
python3 scripts/ctp_daily_reconcile.py \
  --previous artifacts/ctp_reconcile/ctp_reconcile_YYYYMMDD_HHMMSS.json
```

退出码：

```text
0  ready 或 expected_disconnect，HTTP 完整
1  非预期 not-ready / HTTP 错误
```

每日人工检查：

- `phase` 是否符合当前时段；
- `expected_disconnect` 是否在结算/闭市时为 true；
- 持仓与上一交易日收盘后的预期一致；
- 账户 balance/available/margin 无跳变（除结算/出入金）；
- 订单/成交数量与柜台/SimNow 页面一致。

## 5. 交易时段边界

在交易时段内单独运行：

```bash
python3 scripts/ctp_session_soak.py \
  --node-url http://127.0.0.1:18081 \
  --duration 21600 \
  --interval 15 \
  --out artifacts/ctp_session_$(date +%Y%m%d).jsonl
```

观察：

- 09:00 / 10:15 / 10:30 / 11:30 / 13:30 / 15:00；
- 21:00 夜盘开始；
- 节前夜盘是否按 `TradingCalendar` 关闭；
- 结算窗口是否 `expected_disconnect=true`。

## 6. 报单/平仓验收（需要单独确认）

只读周 soak 不报单。交易写测试必须手动加 `--orders --yes`：

```bash
python3 scripts/simnow_acceptance.py \
  --node-url http://127.0.0.1:18081 \
  --symbol SHFE.ao2701 \
  --orders --yes \
  --include-market --include-counterparty \
  --cleanup-market
```

平昨：

```bash
./scripts/ctp_open_overnight_position.sh   # 开仓，必须持有过结算
# 下一交易日：
./scripts/ctp_close_yesterday_test.sh
```

平今/平昨切换要核对：

- SHFE/INE/GFEX 使用 `CLOSETODAY`/`CLOSEYESTERDAY`；
- 非 SHFE 使用通用 `CLOSE`；
- CTP `LimitPrice` 在非 LIMIT 时为 0；
- SHFE ANY/BEST 被转换为 LIMIT+IOC，且 quote 缺失时本地拒绝。

## 7. 故障注入

交易周中可选执行：

```bash
./scripts/ctp_recovery_test.sh
./scripts/ctp_order_fault_test.sh
```

要求：

- gw 重启后 node 不退出；
- IPC resume/replay 或交易 snapshot 对账成功；
- 报单/撤单没有重复提交；
- 恢复后 `/readyz` 回到 true（或当前时段预期断线）。

## 8. 产物归档

保留：

```text
artifacts/ctp_week/
artifacts/ctp_reconcile/
artifacts/ctp_session_*.jsonl
artifacts/simnow_acceptance_*.json
```

不要提交账号、密码、AuthCode、完整 flow 文件或交易流水到仓库。

## 9. 结论模板

```text
交易日范围：
账户/前置：
每日 expected gaps：
unexpected gaps：
非预期 not-ready：
对账差异：
订单/成交/持仓差异：
CTP watchdog/查询异常：
恢复测试结果：
结论：PASS / CONDITIONAL / FAIL
备注：
```
