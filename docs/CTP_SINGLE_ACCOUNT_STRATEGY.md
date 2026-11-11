# 单 SimNow 账户 CTP 量化测试

## 1. 当前约束

只有 1 个 SimNow 账户，因此不能完成双账户的：

- 两个账户独立资金/持仓；
- 跨账户目标仓位隔离；
- 双账户同时断线/重连；
- 两个账户分别执行相反策略。

单账户仍然可以完成完整链路：

```text
Tq MD K线/行情
    -> node 策略脚本
    -> /task/target 或 /order
    -> CTP Td
    -> SimNow 成交/持仓/账户
```

## 2. 2026-11-11 实际状态

已启动 `run_live.sh --no-ui`（Tq MD + CTP Td），并运行：

```bash
python3 scripts/simnow_acceptance.py \
  --node-url http://127.0.0.1:18081 \
  --symbol SHFE.rb2701 \
  --orders --dry-run
```

结果：

- healthz / account / positions / orders / trades / quote / websocket：结构检查通过；
- `readyz`：CTP 处于 `phase=settlement`，`td.connected=false`，
  `expected_disconnect=true`；
- 因此没有下单，也没有伪造成交。

报告：

```text
artifacts/simnow_acceptance_20261111_180856.json
artifacts/simnow_acceptance_20261111_180856.md
```

## 3. 下一个可交易时段

1. 确认 CTP 交易/VPN 网络可达；
2. 启动：

```bash
./scripts/run_live.sh --no-ui
```

3. 等待：

```bash
curl -s http://127.0.0.1:18081/readyz
```

必须是 `ready:true`（不是结算窗口的 503）。

4. 只读 + 订单预演：

```bash
python3 scripts/simnow_acceptance.py \
  --node-url http://127.0.0.1:18081 \
  --symbol SHFE.rb2701 \
  --orders --dry-run
```

5. 真实 1 手 LIMIT/撤单验收：

```bash
python3 scripts/simnow_acceptance.py \
  --node-url http://127.0.0.1:18081 \
  --symbol SHFE.rb2701 \
  --orders --yes
```

6. 策略信号检查（不下单）：

```bash
python3 scripts/strategy_execute.py \
  --node-url http://127.0.0.1:18081 \
  --symbol SHFE.rb2701
```

7. 只有确认账户、合约、方向、手数后才执行：

```bash
python3 scripts/strategy_execute.py \
  --node-url http://127.0.0.1:18081 \
  --symbol SHFE.rb2701 \
  --execute --volume 1
```

脚本只在最新 4h 信号为 long/short 时提交 `TargetPosTask`；`flat` 不提交。
默认扫描使用 `KQ.i@` 指数，执行必须使用实际合约。

## 4. 安全要求

- `config/live.local.env` 必须是 `0600`；
- 不要把账号密码写入命令行历史、PR、Issue 或截图；
- 每个测试场次结束后核对 positions/orders/trades，并清理未成交挂单；
- 单账户结果必须标注为“单账户链路通过”，不能替代双账户验收。
