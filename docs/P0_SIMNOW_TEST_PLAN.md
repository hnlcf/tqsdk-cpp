# P0 SimNow 验收清单

目标：在真实 CTP SimNow 上验证 P0 三项能力：

1. 高级报单：FAK/FOK、市价 `limit_price=0`、BEST、FIVELEVEL、MIN/ALL 数量条件；
2. 执行任务：`TargetPosTask`、`InsertOrderTask`；
3. 风控：开仓次数/手数/报单频率限制，拒绝原因可观测。

> 先决条件：SimNow 已登录、交易阶段非结算、`/readyz` 为 200。
> 任何写测试都先用 1 手、远月/远价；不要留隔夜仓。

## 1. 启动

```bash
cd /home/hnlcf/dev/tqsdk_cpp
cmake --build build-ui --target tqsdk-gw tqsdk-node -j
./scripts/run_live.sh --no-ui   # 另开终端保持运行
```

检查：

```bash
curl -s http://127.0.0.1:18081/healthz
curl -s http://127.0.0.1:18081/readyz
curl -s http://127.0.0.1:18081/catalog/status
```

当前实现的风控/REST 验收接口：

```text
GET    /risk/status
POST   /risk/rules
DELETE /risk/rules
POST   /task/target
DELETE /task/target/{id}
POST   /task/order
DELETE /task/order/{id}
GET    /tasks
```

## 2. 本地 Sim 预演（不碰 SimNow）

先用 Tq 行情 + 本地 SimTd 跑通 REST/任务/风控链路：

```bash
./build-ui/tqsdk-node -c config/p0_simnode.toml

python3 scripts/p0_simnow_acceptance.py \
  --node-url http://127.0.0.1:18088 \
  --symbol SHFE.rb2701 \
  --write --yes --target-write
```

该场景会实际成交 1 手 Sim 仓位；探针结束会撤任务，人工确认后用
`/order` 平掉，或重启 Sim node 重置内存账户。

2026-11-09 本地预演已通过：advanced.fak / advanced.fok / risk.open_count_limit /
task.target.create / task.target.list / task.target.write / tasks 全部 PASS；
Sim 仓位已通过 SELL CLOSE 平掉，`/positions` 为空。
报告归档在 `artifacts/p0_sim_preflight/`。

## 3. 自动探针（推荐）

只读：

```bash
python3 scripts/p0_simnow_acceptance.py \
  --node-url http://127.0.0.1:18081 \
  --symbol SHFE.rb2701
```

写场景（1 手、远价、自动撤单/清理）：

```bash
python3 scripts/p0_simnow_acceptance.py \
  --node-url http://127.0.0.1:18081 \
  --symbol SHFE.rb2701 \
  --write --yes
```

额外验证 TargetPosTask 发单（先用小目标，脚本结束会撤任务；仍需人工确认）：

```bash
python3 scripts/p0_simnow_acceptance.py \
  --node-url http://127.0.0.1:18081 \
  --symbol SHFE.rb2701 \
  --write --yes --target-write
```

报告写入 `artifacts/p0_simnow_*.md/json`。

## 4. 高级报单验收

### 4.1 FAK / FOK

对支持 FAK/FOK 的合约，用远价限价避免成交，验证：

- REST 返回订单号；
- `/orders` 中 `advanced`、`time_cond=IOC`、`volume_cond` 对应 FAK=ANY、FOK=ALL；
- 若未成交，订单应在 IOC 语义下进入终态；
- 不能残留活动委托。

手工示例：

```bash
curl -s -X POST http://127.0.0.1:18081/order \
  -H 'content-type: application/json' \
  -d '{
    "instrument_id":"SHFE.rb2701",
    "direction":"BUY",
    "offset":"OPEN",
    "price_type":"LIMIT",
    "limit_price":3000,
    "advanced":"FAK",
    "volume":1
  }'
```

FOK 将 `advanced` 改为 `FOK`；检查 `/orders` 的订单终态和 `last_msg`。

### 4.2 市价单

```json
{
  "price_type": "ANY",
  "limit_price": 0,
  "time_cond": "IOC",
  "volume": 1
}
```

SHFE/INE 上按现有 normalizer 转成涨跌停限价 + IOC；其他交易所由 CTP 处理。
市价单风险更高，验收前确认当前为 SimNow，且只下 1 手。

### 4.3 BEST / FIVELEVEL

- `BEST`：对价，按买一/卖一（或 CTP 最优价）成交；
- `FIVELEVEL`：五档价，官方接口只在 CFFEX 场景可用；
  - 用 `CFFEX.IF`/`CFFEX.IC` 等合约验证；
  - 非 CFFEX 合约不要断言成功，CTP 可能直接拒绝。

## 5. 执行任务验收

### 5.1 TargetPosTask

先用 `target_volume=0` 验证生命周期，再在人工确认后使用 1 手：

```bash
curl -s -X POST http://127.0.0.1:18081/task/target \
  -H 'content-type: application/json' \
  -d '{"symbol":"SHFE.rb2701","target_volume":0,"mode":"ACTIVE"}'

curl -s http://127.0.0.1:18081/tasks
```

写场景只允许在 SimNow 小目标下执行；脚本会自动撤任务，但**任务撤单后仍需人工确认持仓**。
重点观察：

- `/tasks` 的 target task 活跃状态；
- `/positions` 的净持仓是否朝目标收敛；
- 任务只允许一笔在途单，不重复报单；
- 任务取消后活动委托被撤。

### 5.2 InsertOrderTask

通过 `POST /task/order` 提交一个限价单任务；或使用 C++ 测试：

```bash
ctest --test-dir build-ui -R OrderTasks --output-on-failure
```

## 6. 风控验收

### 6.1 开仓次数限制

```bash
curl -s -X POST http://127.0.0.1:18081/risk/rules \
  -H 'content-type: application/json' \
  -d '{"type":"OPEN_COUNT_LIMIT","limit":1,"symbols":["SHFE.rb2701"]}'
```

提交两笔远价开仓单，第二笔应返回：

```json
{"success": false, "error": "触发风控：合约 SHFE.rb2701 交易日内开仓次数上限 1"}
```

清理：

```bash
curl -s -X DELETE http://127.0.0.1:18081/risk/rules
```

### 6.2 手数/频率

```json
{"type":"OPEN_VOLUME_LIMIT","limit":2,"symbols":["SHFE.rb2701"]}
{"type":"ORDER_RATE_LIMIT","limit":5,"window_ms":1000}
```

确认拒绝发生在网关之前（`/orders` 中出现 `is_error=true`，不能出现在 CTP 委托查询中）。

## 7. 验收通过标准

- [ ] `/readyz` 200，catalog loaded；
- [ ] FAK/FOK 字段和订单终态正确，无残留挂单；
- [ ] 市价/BEST/FIVELEVEL 的 `limit_price=0` 路径正确；
- [ ] TargetPosTask 朝净目标收敛，无重复在途单；
- [ ] 风控拒绝原因可见且不落到柜台；
- [ ] 所有测试订单已撤、风险规则已清理、任务已删除；
- [ ] `artifacts/p0_simnow_*.md` 已生成并归档。

## 8. 已知限制

- TargetPosTask v1 不自动追价/撤单重报；
- 风控按中国时区自然日近似交易日，夜盘跨交易日场景后续接入 `TradingCalendar`；
- FIVELEVEL 只在 CFFEX 合约断言成功；
- SimNow 的可用合约、夜盘和交易阶段会变化，脚本以 `/readyz` 和 `/quote` 为准，不假定固定合约一定可交易。
