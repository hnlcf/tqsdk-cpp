# SimNow / CTP 验收检查

`scripts/simnow_acceptance.py` 用于在 SimNow 上做可重复的端到端验收，避免
晚上手工点单后无法回溯“哪一步过了、哪一步失败”。

## 1. 启动环境

先准备 CTP/SimNow 配置：

```bash
cp config/live.local.env.example config/live.local.env
chmod 600 config/live.local.env
$EDITOR config/live.local.env
```

启动 gw/node，不启动 UI：

```bash
./scripts/run_live.sh --no-ui
```

默认 node：

```text
http://127.0.0.1:18081
```

也可以连接其他 node：

```bash
python3 scripts/simnow_acceptance.py --node-url http://127.0.0.1:18081
```

## 2. 只读检查

默认不会下单：

```bash
python3 scripts/simnow_acceptance.py \
  --symbol SHFE.rb2701 \
  --metrics
```

覆盖：

- `/healthz`；
- `/readyz`（最长等待 60 秒）；
- `/account`；
- `/positions`、`/orders`、`/trades`；
- `/quote/{symbol}`；
- 五档/一档盘口识别；
- `/metrics`（可选）；
- WebSocket `/stream` 订阅欢迎、订阅确认和快照（默认启用，可用 `--no-ws` 关闭）；
- quote 首次访问会触发新订阅，脚本默认等待 `--quote-timeout 15s`，新合约没有行情时会明确失败而不是立即误报。

## 3. 限价/撤单/错误单验收

写操作必须显式确认：

```bash
python3 scripts/simnow_acceptance.py \
  --symbol SHFE.rb2701 \
  --orders --yes
```

默认行为：

1. 读取 quote 和 `price_tick`；
2. 计算远离盘口的限价（BUY 低于买一，SELL 高于卖一），尽量避免成交；
3. 提交 `BUY OPEN` 小单；
4. 查询 `/orders` 确认订单可见；
5. 立即撤单并等待终态；
6. 提交 `volume=0` 的错误单，验证拒绝链路。

如果限价单被立即成交或进入终态，报告会标记 `WARN`，因为此时无法验证撤单。

只预览订单，不提交：

```bash
python3 scripts/simnow_acceptance.py --orders --dry-run
```

`--orders` 不带 `--yes` 时会直接拒绝执行，避免误下真实 SimNow 单。

## 4. 市价/对价检查

市价单和对价单可能立即成交并产生持仓，默认 **不执行**。确认可以接受后显式开启：

```bash
python3 scripts/simnow_acceptance.py \
  --symbol SHFE.rb2701 \
  --orders --yes \
  --include-market \
  --include-counterparty \
  --cleanup-market
```

- 市价：`price_type=ANY`；
- 对价：`price_type=BEST`；
- 使用 `time_cond=IOC`；
- SHFE 会在 node 侧转换成合法限价单：
  - 市价 BUY -> 涨停价；
  - 市价 SELL -> 跌停价；
  - 对价 BUY -> 卖一价；
  - 对价 SELL -> 买一价；
- 没有有效 quote/停板价时，下单直接返回错误；
- `--cleanup-market` 只平掉相对测试前新增的净持仓，不会碰原有持仓；
- 不带 `--cleanup-market` 时脚本不会自动平仓，需要人工确认；
- 可选检查被柜台/交易所拒绝会记录 `WARN`，而不是误报成“下单成功”。

## 5. 报告

默认写入：

```text
artifacts/simnow_acceptance_YYYYmmdd_HHMMSS.json
artifacts/simnow_acceptance_YYYYmmdd_HHMMSS.md
```

可以指定路径：

```bash
--json-out artifacts/my_acceptance.json
--md-out artifacts/my_acceptance.md
```

退出码：

```text
0  没有 FAIL（WARN/SKIP 允许）
1  存在 FAIL
2  参数/安全确认失败
```

报告中的每条检查都有 `PASS/FAIL/WARN/SKIP`，并附带原响应证据，便于晚上复盘。

## 6. 自测

验收脚本本身有一个本地 fake node 测试，不会连接 SimNow：

```bash
python3 scripts/test_simnow_acceptance.py
```

它覆盖：

- health/ready/account/positions/orders/trades/quote；
- 限价提交和撤单；
- volume=0 错误拒绝；
- JSON/Markdown 报告生成。

## 7. 当前边界

- 脚本不启动 gw/node，复用 `run_live.sh --no-ui`；
- 暂不自动验证 CTP 今仓/昨仓平仓策略，平仓验收需要按持仓情况手工执行；
- 暂不自动重启 gw 做 reconnection 断言，可通过人工重启后再次运行只读检查；
- 真实 SimNow 验收仍应以柜台回报、成交/持仓查询结果为准，脚本只做自动化探针。

## 8. 恢复与结算专项

### 8.1 gateway / node 重启

`scripts/ctp_recovery_test.sh` 会直接启动 gw/node（避免 `run_live.sh` 在 gw
退出时连带关闭 node），然后执行：

1. 杀掉 gw；
2. 确认 node 仍存活，`/readyz` 返回 503；
3. 重启 gw，等待 CTP 登录、结算确认和初始查询快照；
4. 重启 node；
5. 对比 account/positions/orders。

实测：

```text
gateway down: readyz=503, node_alive=1
gateway restart: ready=true, 约 8s
node restart: ready=true, 约 2s
positions/account/orders before/after 一致
```

建议使用隔离端口和隔离 `flow_path` 跑，避免旧 gw 仍占用端口造成假阳性：

```bash
NODE_URL=http://127.0.0.1:28081 \
GW_CONFIG=/tmp/ctp_recovery_gw.toml \
NODE_CONFIG=/tmp/ctp_recovery_node.toml \
./scripts/ctp_recovery_test.sh
```

隔离端口实测（2026-11-05）：

```text
initial ready: 7s
gw down: readyz=503, node alive
gw restart: 8s
node restart: 2s
```

本次恢复测试的 raw log 已保存到
`artifacts/ctp_recovery_20261105_1048.log`。

### 8.2 平昨仓

平昨必须跨过 SimNow 每日结算，不能用连续 24h 测试代替。验收流程：

```bash
# 交易日开一个 1 手 SHFE.rb2701 测试多仓，不自动平仓
./scripts/ctp_open_overnight_position.sh

# 当日结算后（下一交易日）
./scripts/ctp_close_yesterday_test.sh
```

第二个脚本要求 `volume_long_his >= 1`，用 `Offset=CLOSE` 发出平仓单，并检查
`volume_long_his` 减少。当前已经开好测试仓：

```text
SHFE.rb2701 long=1 today=1 his=0
pending: artifacts/ctp_close_yesterday_pending.json
```

尚未执行“次日平昨”本身，不能标记为 PASS。

### 8.3 部分成交

部分成交取决于 SimNow 撮合和盘口深度，当前活跃合约盘口经常一次性全部成交，
不属于稳定可复现的验收场景。UI 侧已用模型测试覆盖 `volume_left/volume_origin`
和回报消息展示；真实 CTP 部分成交需要专门的低流动性合约/撮合窗口，暂不伪造
PASS。

### 8.4 结算约束

CTP 交易账户不是 24h 可用服务：每日结算期间前置会断开，仓位和结算数据在
下一交易日恢复。因此 CTP 不能用“连续 24h 不断线”作为验收标准，应改为：

```text
交易时段 soak
  -> 收盘/结算前记录快照
  -> 允许并验证预期断线
  -> 下一交易时段重连
  -> 对账 positions/orders/trades/account
```

当前后台 24h soak 是 SimTd/Ipc 稳定性测试，不是 CTP 连续在线测试。

### 8.5 自动平昨任务

已准备并提交 `scripts/ctp_overnight_close_runner.sh`，它会在隔离端口启动
gateway/node，等待 `/readyz`，然后调用平昨脚本并清理进程。

当前测试仓：

```text
SHFE.rb2701 long=1 today=1 his=0
pending: artifacts/ctp_close_yesterday_pending.json
```

已通过 `at` 安排下一交易日 09:30 自动执行（隔离端口 24332/28083）。如果执行
成功，日志会写到：

```text
artifacts/ctp_overnight_close_runner.log
```

如果届时仍没有历史仓，脚本会明确退出并保留仓位，不会发送错误的平仓单。

### 8.6 报单在途 gateway 故障

`scripts/ctp_order_fault_test.sh` 已实测：

1. 提交远离盘口的 BUY LIMIT，订单保持 ALIVE；
2. 杀掉 gateway，node 不退出；
3. 重启 gateway，订单从 CTP/订单缓存恢复；
4. 撤销恢复后的订单，确认终态“已撤单”；
5. 持仓数量前后不变。

本次结果：

```text
order_before_kill: ALIVE, left=1
gateway restart:   6s
order_after_restart: ALIVE, 同一 order_id
order_final:        FINISHED / 已撤单
positions:          前后均只有既有测试仓
```

原始日志：`artifacts/ctp_order_fault_20261105.log`。

### 8.7 交易时段监控

当前 CTP session monitor 正在运行：

```text
artifacts/ctp_session_20261105_live.jsonl
```

它按 30s 采样 `/readyz` 的 `phase` 和 account/positions/orders；结算断线会
记录为 expected，而不是 fatal。

### 8.8 部分成交探针

`scripts/ctp_partial_fill_probe.sh` 采用小额、自动清理的方式观察 SimNow 是否能
稳定构造部分成交：

- 只选择盘口卖一量较小且价格低的期权合约；
- 请求量 = 卖一量 + 1，最多 40 手；
- 记录 `PARTIAL/FULL/NO_FILL`，不把封盘拒绝伪装成 PASS；
- 若成交则立即以买一价平仓，退出时撤销残余订单。

由于部分成交取决于 SimNow 撮合窗口，这个探针是“可重复尝试”，不是每次都能
得到 PARTIAL。

### 8.9 后续自动探针

- 13:35 部分成交 probe：小额期权、自动撤余量/平仓，结果可能为
  `PARTIAL/FULL/NO_FILL`；
- 09:30/10:30 平昨 runner；
- 10:35 下一交易时段 session monitor。

这些任务使用 `artifacts/` 下的独立日志/JSONL，不覆盖本次报告。

### 8.10 2026-11-05 13:35 实际结果

盘前/午休时段提交期权单曾收到：

```text
26: 已撤单报单被拒绝 SHFE:当前状态禁止此项操作
```

午后开盘后重新执行小额期权 probe，三笔分别请求 40/60/72 手，SimNow 均
返回：

```text
RESULT=FULL
```

并自动以买一价平仓，最终期权持仓为 0、无未成交单。因此当前 SimNow 撮合环境
仍无法稳定构造部分成交；这属于撮合环境限制，没有伪造 `PARTIAL` 结果。

### 8.11 2026-11-05 13:36 午后复验

开盘后再次执行完整写测试（`--orders --include-market --include-counterparty
--cleanup-market`）：

```text
artifacts/simnow_acceptance_20261105_133656.json
PASS 15 / FAIL 0 / WARN 0 / SKIP 0
```

当时 quote：

```text
SHFE.ao2701 last=2632.0 tick=1.0
```

市价/对价均成交并由 cleanup 平掉；最终没有残留活动订单，`CZCE.SA701` 和
`CZCE.SR701` 原持仓未变，仍保留 `SHFE.rb2701` 1 手平昨测试仓。

### 8.12 平昨最终结果

2026-11-06 09:30 自动平昨成功：

```text
SHFE.rb2701 long=1
volume_long_his: 1 -> 0
order: r1793928600
status: 全部成交报单已提交
after position: 0
```

10:30 重试按预期检测不到历史测试仓，没有再次报单。测试仓已清理，用户原有
SA/SR 仓位未修改。
