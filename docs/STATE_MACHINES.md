# 状态机

状态机描述“何时允许什么操作、异常如何恢复”。跨模块数据流见
[FLOWS.md](FLOWS.md)。

## 1. CTP TD 生命周期

```text
Stopped
  │ start()
  ▼
Connecting
  │ OnFrontConnected
  ▼
Authenticate (可选，取决于 AppID/AuthCode)
  │ OnRspAuthenticate OK
  ▼
Login
  │ OnRspUserLogin OK
  ▼
SettlementConfirm
  │ OnRspSettlementInfoConfirm OK
  ▼
InitialQuery
  │ instruments/account/positions/orders/trades 查询完成
  ▼
Ready
  │ OnFrontDisconnected
  ▼
Reconnecting ── CTP API 自动重连 ──> Connecting
```

规则：

- `requires_initial_trade_snapshot()=true` 的远端网关，在 Ready 前
  `/readyz` 不能为 true；
- `snapshot_ready_` 只在最新一轮查询完成后置 true；
- `Disconnected` 必须清掉 `snapshot_ready_`，避免旧截面被当成新快照；
- 结算阶段断线不是故障；`session_phase()` 应优先返回 `Settlement`；
- 交易时段断线返回 `Reconnecting`。

## 2. Node readiness

```text
Disconnected
  │ IPC connect + Hello OK
  ▼
Connecting
  │ remote gateways up
  ▼
WaitingSnapshot
  ├─ md ready?
  ├─ td snapshot ready?（如要求）
  ▼
Ready
  ├─ md/td disconnect -> NotReady
  └─ close -> Closed
```

`/readyz` 输出：

```json
{
  "ready": true,
  "md": {"present": true, "connected": true, "ready": true},
  "td": {
    "present": true,
    "connected": true,
    "ready": true,
    "phase": "open",
    "expected_disconnect": false
  }
}
```

- `md.present=false` 表示该部署没有 MD；`md.ready=true`；
- 结算阶段可以 `ready=false` + `expected_disconnect=true`；
- HTTP 状态码：ready ? 200 : 503。

## 3. IPC Session 状态

```text
New
  │ start()
  ▼
Handshaking
  ├─ bad role/name -> Failed
  ▼
Ready
  │ heartbeat timeout / peer close / local close
  ▼
Closed
```

### Seq/Replay

```text
normal:
  producer next_seq=N -> receiver expects N -> accept -> N+1

gap:
  receiver expects N, got M>N
    -> request Resume{epoch,next_seq=N}
    -> producer replays [N,M)
    -> if possible: recover
    -> else: resync/reconcile
```

`epoch` 变化表示对端进程重启，必须 resync，不能跨 epoch replay。

## 4. Order 状态机

```text
LocalPlaceholder (ALIVE)
  ├─ gateway ack success
  │    ├─ 未成交 -> ALIVE
  │    ├─ 部分成交 -> ALIVE (volume_left>0)
  │    └─ 全部成交 -> FINISHED (is_dead=true)
  ├─ gateway reject
  │    └─ FINISHED (is_error=true)
  └─ transport/ack exception
       └─ FINISHED (is_error=true, last_msg=...)
```

关键约束：

- `insert_order` 返回本地占位指针，立即对 UI 可见；
- ack 只是命令层，不表示终态；
- 终态由 TradeEvent Diff 或本地错误路径推进；
- 撤单成功后本地订单应收到 `FINISHED / 已撤单`；
- 同一个本地 rid 不应重复提交到 CTP（IPC CommandAckCache + CTP OrderRef 保障）。

## 5. WebSocket 客户端状态

```text
Disconnected
  │ connectToNode
  ▼
Connecting
  │ WS connected
  ▼
Subscribed
  ├─ subscribe ack
  ├─ snapshot
  ▼
Synced
  ├─ diff -> Synced
  ├─ disconnect -> Reconnecting
  └─ close -> Disconnected
```

订阅状态：

```text
topics 缺省
  -> quote/kline only
topics 显式
  -> 按列出的 account/orders/trades/positions
```

客户端收到 `snapshot` 后，模型必须整体替换；收到 `diff` 后按 seq 应用。
服务端可能丢慢消费者最旧消息；客户端重连后靠 snapshot 恢复完整状态。

## 6. TradingCalendar 状态

```text
Unknown
  ├─ weekend/holiday -> Closed
  ├─ pre-open window -> PreOpen
  ├─ session -> Open
  ├─ between day sessions -> Break
  ├─ day close..settlement end -> Settlement
  └─ disconnected during expected phase -> Reconnecting/expected
```

判定顺序（当前实现）：

1. 跨零点夜盘：检查前一日是否交易日 + 夜盘是否允许；
2. 非交易日（周末/节假日）-> Closed；
3. 日盘/夜盘交易窗口 -> Open；
4. 日盘段间 -> Break；
5. 15:00–20:30 -> Settlement；
6. 开盘前窗口 -> PreOpen；
7. 其余 -> Closed。

节前夜盘：

```text
night_session_allowed(day)
  scan day+1 .. next trading day
  holiday encountered -> false
  weekend skipped -> true at next weekday
```

## 7. HolidayCalendar 状态

```text
Unloaded
  ├─ local cache exists -> Loaded(local)
  └─ no cache -> Downloading
                    ├─ success -> Loaded(remote)
                    └─ failure -> Unloaded (保留旧版本)
Loaded
  ├─ refresh interval -> Downloading
  ├─ HTTP failure -> Loaded(old version)
  └─ stop -> Stopped
```

下载采用临时文件 + rename；进程级缓存路径 `/tmp/tqsdk_holidays.json`。
同一进程 node/gw 各自加载一份，缓存文件是进程间共享的只读快照。

## 8. KlineStore 状态

```text
Disabled -> Idle
Enabled
  ├─ writer thread ready -> Ready
  ├─ pending bars -> Flushing
  └─ flush failure -> Degraded（保留内存 K 线）
```

KlineStore 的失败不能阻塞行情合并；缓存只是加速恢复，不是唯一数据源。

## 9. 状态相关测试

```text
tests/gateway/ctp_fault_injection_test.cpp
tests/ipc/gateway_server_test.cpp
tests/ipc/command_ack_cache_test.cpp
tests/ipc/gateway_subscription_book_test.cpp
tests/serve/readiness_test.cpp
tests/serve/ws_stream_test.cpp
tests/serve/ws_entity_filter_test.cpp
tests/trading/trading_calendar_test.cpp
tests/trading/tqapi_gateway_fault_test.cpp
```
