# 信息流与模块交互

本文按运行场景描述“数据从哪来、经过谁、最终到哪”。状态迁移见
[STATE_MACHINES.md](STATE_MACHINES.md)，协议字段见
[PROTOCOLS.md](PROTOCOLS.md)。

## 1. 启动与就绪

```text
gw_main
  ├─ 解析 GatewayConfig
  ├─ 创建唯一 TqAuth（账号密码只在 gw）
  ├─ 创建真实 MD/TD 网关
  ├─ 创建 GatewayHost(md, td, auth service)
  ├─ 启动 CTP TaskQueue / Tq MD
  ├─ listen IPC TCP/UDS
  └─ 等待 node 连接

node_main
  ├─ 解析 NodeConfig（无 tq_auth）
  ├─ 连接 IPC，握手 (Hello)
  ├─ make_engine_runtime -> RemoteMd / RemoteTd + RemoteAuthService
  ├─ 通过 IPC ServiceRequest(auth::snapshot) 取只读认证快照
  ├─ 启动 InstrumentCatalog / HolidayCalendar / KlineStore
  ├─ TqApi.start()
  ├─ 等待初始行情/交易 snapshot
  ├─ HttpServer.listen()
  └─ /readyz: ready=true

TQSDK Board (Web)
  ├─ createMarketStream + REST client
  ├─ GET /readyz / WS /stream
  ├─ subscribe symbols + klines [+ topics]
  ├─ snapshot -> 模型
  └─ diff -> 模型/图表
```

### `/readyz` 语义

`ready=true` 需要：

- md 不存在或 md ready；
- td 不存在或初始 snapshot 完成/不要求 snapshot；
- 进程未 closed。

TD 的 `phase` 与 `expected_disconnect` 用于区分交易时段和结算断线。

## 2. 行情数据流

### Tq 行情

```text
Tq WebSocket frame
  -> TqMdGateway
  -> MarketDataEvent{Diff/Kline}
  -> RemoteMdGateway (gw/node IPC)
  -> TqApi.md_loop
  -> DiffMergeEngine
  -> DataManager.quotes / klines
  -> WebSocketHub.push_loop
  -> WsEntityFilter (按客户端 symbols/topics)
  -> WS diff
```

### CTP 行情（如果 md.kind=ctp）

```text
CTP MdApi 回调线程
  -> CtpTaskQueue
  -> Asio gateway strand
  -> CtpMdGateway
  -> MarketDataEvent
  -> 后续同上
```

### K 线

```text
subscribe_kline
  -> 上游网关订阅
  -> KlineStream(DataManager)
  -> KlineStore.async writer（可选）
  -> WS kline diff / REST /kline/{symbol}/{duration}
  -> UI MarketStore + KLineChartWidget
```

K 线历史缓存只在 node 启动/首次需要时 hydrate；SQLite 不进入行情合并线程。

## 3. 交易数据流

### 下单

```text
TQSDK Board OrderTicket
  -> POST /order
  -> POST /order
  -> TqApi.insert_order
     ├─ 创建本地占位 Order（立即可见）
     ├─ CTP/SHFE 价格归一化（如需）
     └─ TradingGateway.insert_order
          -> RemoteTdGateway
          -> IPC Command
          -> GatewayIpcServer.handle_command
          -> CtpTdGateway / SimTdGateway
          -> CommandAck
  -> HTTP response {success, order_id, error}
```

### 订单/成交事件

```text
CTP OnRtnOrder/OnRtnTrade
  -> CtpTaskQueue
  -> CtpTdGateway
  -> TradeEvent::Diff
  -> GatewayIpcServer.td_pump
  -> IPC TradePush
  -> RemoteTdGateway
  -> TqApi.td_loop
  -> DiffMergeEngine
  -> DataManager.orders/trades/account
  -> WebSocketHub orders/trades/positions topics
  -> UI OrderTableModel/TradeTableModel
```

### 订单状态

```text
ALIVE -> (部分成交) -> ALIVE
ALIVE -> FINISHED/已撤单/全部成交
local placeholder -> error (CTP reject / transport exception)
```

`TqApi.insert_order` 只保证本地占位立即可见和命令 ack 语义，不原地等待终态。
真正的状态推进必须通过 `wait_update()` + `get_order()`。

## 4. 订阅流

### WS 客户端订阅

```text
client -> subscribe {symbols, klines, topics}
  -> WebSocketHub.add_subscriptions
     ├─ GatewaySubscriptionBook?（node 内不使用）
     ├─ quote_refs 增加（首次 0->1 才向上游 subscribe）
     ├─ kline_refs 增加（首次才 subscribe_kline）
     └─ topics 显式替换；缺省只保留行情
  -> subscribed ack
  -> snapshot（仅已订阅实体）
```

### 服务端推送

```text
TqApi.wait_update
  -> WebSocketHub.build_diff
  -> WsEntityFilter.filter（每客户端）
     ├─ quotes/klines 按订阅裁剪
     ├─ orders/trades/positions/account 只发给显式 topics
     └─ 比较上次 JSON，未变化实体丢弃
  -> WsDiff{seq,timestamp_ns,data}
```

## 5. IPC 断线/重连

```text
IpcClient connection lost
  -> RemoteMdGateway / RemoteTdGateway emit Disconnected
  -> TqApi md_ready/td_ready false
  -> node /readyz false
  -> reconnect with backoff
  -> Hello/HelloAck
  -> Resume{epoch,next_seq}
       ├─ 同 epoch 且连续 -> replay
       └─ epoch 变化/缺口 -> resync + reconcile
  -> Remote gateways replay subscriptions
  -> td snapshot -> TqApi.td_loop 清交易实体后重建
  -> ready=true
```

`GatewaySubscriptionBook` 在 gw 侧对每个连接维护 symbol/K 线引用计数，
断线时只释放该连接的订阅，不影响其他 node。

## 6. 交易重启与对账

```text
gw restart
  -> CTP login + settlement confirm
  -> query instruments/account/positions/orders/trades
  -> CtpOrderStore load persisted orders（同一交易日）
  -> TradeSnapshot
  -> node TqApi 清交易实体并应用快照
  -> /readyz true；WS snapshot/diff 更新 UI
```

跨交易日时 order store 按 `trading_day` 滚转并清旧订单。

## 7. 交易日/节假日

```text
HolidayCalendar.start_global
  -> load /tmp/tqsdk_holidays.json
  -> background refresh URL
  -> dates set

TradingCalendar.phase_at(now)
  -> weekend?
  -> holiday?
  -> early-night belongs previous trading day?
  -> sessions / preopen / break / settlement
  -> phase/expected_disconnect
```

节前夜盘规则：

```text
从当晚向后找下一交易日
  -> 遇节假日 => 当晚夜盘关闭
  => 正常周五夜盘保留
  => 周五后周一放假 => 周五夜盘关闭
```

## 8. HTTP REST 流

```text
GET /quote/{symbol}      -> get_quote + catalog metadata
GET /quotes?symbols=...  -> batched quotes
GET /kline/{symbol}/{ns} -> get_kline_serial + bars
GET /account/orders/trades/positions
GET /trading-snapshot    -> SnapshotService 合并投影
POST /order              -> TqApi.insert_order
DELETE /order/{id}       -> TqApi.cancel_order
GET /readyz              -> readiness + phase + expected_disconnect
GET /holiday/status      -> holiday cache/version
```

HTTP 读接口在 data_ex/core strand 执行；返回前立即序列化为 JSON，不跨
`co_await` 持有 DataManager 引用。

## 9. Web 前端数据流

```text
HTTP/WS bytes
  -> HTTP fetch / WebSocket
  -> SolidJS signal/store merge
     ├─ quote/klines -> chart (command-style series.update)
     └─ account/orders/trades/positions -> tables
  -> chart / components
```

Web 前端只依赖 node REST/WS；K 线历史来源 REST，增量来源 WS。
图表不随每个 tick 重建，Solid 只更新数据信号和图表 series。

## 10. 失败路径

| 失败 | 表现 | 恢复 |
|---|---|---|
| CTP TD 断线 | td_connected=false, `/readyz` false | CTP API 重连/登录/查询 |
| CTP 结算断线 | phase=settlement, expected_disconnect=true | 下一交易时段登录 |
| IPC 断开 | node ready false, WS 保持 | resume/replay/reconcile |
| node HTTP 无响应 | UI reconnect timer | WebSocket 重连 + 订阅回放 |
| WS 慢消费者 | 队列达到 max_queue 丢最旧 | 下一 snapshot/diff |
| ack/transport 异常 | 本地订单错误终态 | 后续对账修正 |
| catalog 未加载 | `/catalog` 503 | 刷新 symbol 文件 |
| holiday 下载失败 | 保留旧缓存 | 下个刷新周期 |
