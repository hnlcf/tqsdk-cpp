# IPC 序号、Resume 与重连恢复

本文描述 gw ↔ node 之间行情（md）/交易（td）两条独立事件流的可靠性机制：
序号、epoch、缺口检测、Resume/replay、行情重订阅，以及交易快照对账。

## 1. 为什么需要它

IPC 底层只保证**帧边界**，不保证业务事件不丢、不重、不乱序：

- TCP/UDS 断线会丢失断线窗口内的 push；
- gw 重启后旧连接上的 seq 基线不再有意义；
- 交易事件不能像行情一样简单“重订阅后拿快照”——必须先清理旧状态再应用完整截面；
- 命令（报单/撤单）本身非幂等，重发必须复用 `rid` 并由 gw 侧去重。

因此协议在每条 push 消息体上增加 `StreamMeta{epoch, seq}`，并引入
`Resume` / `ResumeAck` / `TradeSnapshot` 三种帧。

## 2. 两条独立的流

`StreamKind::Md` 与 `StreamKind::Td` 各自维护：

```text
epoch : gw 进程一次启动内唯一（含时间/pid/计数器）
seq   : 从 1 开始单调递增，每个 push 占一个 seq
```

- md 与 td 的 seq **互不相关**，各自独立检测；
- seq 分配顺序必须等于发送顺序。生产侧每个流只有单个 writer
  （`GatewayIpcServer` 的 `md_pump` / `td_pump`），且 `next_meta()`、
  `record()`、`send()` 在同一串行路径内完成；
- `seq == 0` 表示旧版对端没有提供 meta，接收端按 `Ignored` 处理，保持向后兼容。

### 2.1 接收端状态机（`StreamTracker`）

| 结果 | 条件 | 处理 |
|---|---|---|
| `Ignored` | `seq == 0` | 直接交付，不建立基线 |
| `Started` | 首次收到有效 seq | 建立 `expected = seq + 1` |
| `EpochChanged` | epoch 变化 | 重置基线，计数 +1 |
| `InOrder` | `seq == expected` | `expected++` |
| `Duplicate` | `seq < expected` | 丢弃，计数 +1 |
| `Gap` | `seq > expected` | 触发 `GapHandler`，记录 `expected/received/missing` |

`StreamGap{kind, epoch, expected, received, missing}` 会同时写入日志并回调上层。
`IpcClient.metrics()` 暴露每条流的帧数、缺口数、重复数、epoch 变化数、Resume 次数。

## 3. 生产端：OutboundStream + replay buffer

每个 `GatewayIpcServer` 持有 `OutboundStream md_stream_ / td_stream_`：

- `next_meta()` 分配 seq；
- `record(seq, type, encoding, payload)` 把已发送帧写入有界 ring buffer；
- `replay_from(next_seq, out)` 从 buffer 中取 `seq >= next_seq` 的帧。

容量可配置：

```cpp
GatewayIpcServer::Options {
    ReplayConfig md_replay{4096, 16MB};
    ReplayConfig td_replay{16384, 64MB};
};
```

若 `next_seq` 已被淘汰，`replay_from` 返回 `false`，表示必须走更重的恢复路径。
md 的恢复是“重订阅 + 全量快照”；td 的恢复是 `TradeSnapshot` 全量对账。

## 4. 断线与重连流程

### 4.1 node 侧 `IpcClient`

可重连模式由 `IpcClient(SessionFactory, executor, ReconnectOptions)` 构造：

1. `connect()` 建立**首条**连接并完成握手；失败直接抛错（启动 fail-fast）；
2. `start()` 启动连接监督循环；
3. 连接断开后按指数退避重连（默认 200ms → 5s 上限）；
4. 重连成功后：
   - 调用 `ReconnectHandler`（`RemoteMdGateway` 在这里重放订阅）；
   - 发送 `Resume`：
     - `epoch` 取 md/td tracker 已知 epoch；
     - `md_next_seq` 取 md 期望序号（没有基线则为 0）；
     - `td_next_seq` 取 td 期望序号；若没有基线则填 1，强制 gw 返回 td 快照；
5. 旧 session 仍可能有 heartbeat/watchdog 协程在收尾，因此放入
   `retired_sessions_` 保留到 `IpcClient` 析构，避免 UAF。

### 4.2 gw 侧 `GatewayIpcServer::handle_resume`

- 请求 epoch 与当前 `stream_epoch_` 相同：
  - md：`replay_from(md_next_seq)`；失败则 ack `md_replay=false`；
  - td：`replay_from(td_next_seq)`；失败且 `td_ != nullptr` 则
    `td_snapshot_required=true`；
- 请求 epoch 不同（gw 重启过）：
  - 不 replay；
  - `md_from_seq=0`，让 node 重放订阅；
  - `td_from_seq=1`、`td_snapshot_required=true`（若 gw 有交易网关），
    直接把 node 拉到新的交易截面；
- ack 先发，replay 帧随后按原顺序发送；需要时最后发送 `TradeSnapshot`。

### 4.3 md 缺口 → resync

`RemoteMdGateway` 维护已订阅的 `symbols_` 与 `klines_`：

- `subscribe/subscribe_kline` 成功后记录；
- `release` 成功后移除；
- `resync()` 用当前集合重新发送一条 `SubscribeBody`；
- md gap handler / reconnect handler 都会 `co_spawn(resync)`。

重订阅会触发上游全量快照，`TqApi` 的 `DiffMergeEngine` 用完整实体覆盖旧值；
旧 seq 基线在 gap/ResumeAck 时重置，因此新快照的第一帧会重新建立基线。

### 4.4 td 缺口 → replay 或 reconcile

td 不允许静默丢事件。`RemoteTdGateway` 的 gap handler 会发送
`Resume{epoch=gap.epoch, td_next_seq=gap.expected}`：

- gw buffer 仍覆盖缺口：replay 缺失帧，事件按原 seq 应用；
- buffer 已淘汰：ack 要求快照，gw 发送 `TradeSnapshot`；
- node 收到 `TradeSnapshot` 后：
  - 先 `DataManager::clear_trading()` 清空 orders/trades/positions 并重置 account，
  - 再把快照 `trade` 树交给 `DiffMergeEngine::apply()`，
  - 最后 `notify()`，让等待者看到一致的“新截面”。

这样不会出现“旧订单 + 新快照”混合的中间状态。

## 5. 交易网关快照契约

`TradingGateway::snapshot()` 返回一个可直接被 `DiffMergeEngine` 应用的
`{"trade":{"":{"user":{orders,trades,positions,accounts}}}}` 结构。

- `SimTdGateway` 维护 `order_history_` / `trade_history_`，并把当前
  账户/持仓/订单/成交拼成完整截面；
- 默认实现返回空对象，表示“该网关不支持快照”；
- `CtpTdGateway` 用 `ReqQryOrder/Trade/InvestorPosition/TradingAccount` 构造快照，
  查询完成前保留上一份可用截面；
- 命令重试已落地：`IpcClient` 超时后用同一 `rid` 重发，`GatewayIpcServer` 按
  `<op>:<rid>` 缓存首次 ack，命中时不再次调用网关；
- 该缓存是**每连接**的；连接重建后的跨会话幂等仍要靠订单查询/人工对账，
  不能简单重发报单。

## 6. 兼容性

- 旧 peer 不发送 `meta`：`StreamMeta` 的 `from_json` 默认 `seq=0`，接收端 `Ignored`；
- 旧 peer 不认识 `Resume/ResumeAck/TradeSnapshot`：新代码只在重连/缺口时发送，
  正常首连不会主动发 `Resume`；
- `TradeSnapshot` 是 `TradePushBody` 之外的独立帧类型，旧 peer 会按未知帧忽略/报错，
  因此跨版本混跑时应先升级 gw。

## 7. 观测与测试

日志关键字：

- `[IpcClient] disconnected; reconnect in ...`
- `[IpcClient] reconnect failed: ...`
- `[IpcClient] resume: epoch=... md_next=... td_next=...`
- `Md seq gap: ...` / `Td seq gap: ...`
- `[RemoteMd] resync: N symbols, M klines`
- `[RemoteTd] seq gap ...; request replay`

测试覆盖：

- `tests/ipc/stream_seq_test.cpp`：Tracker 状态机、replay/淘汰；
- `tests/ipc/protocol_test.cpp`：`StreamMeta`、`Resume`、`ResumeAck`、`TradeSnapshot` 编解码；
- `tests/ipc/gateway_server_test.cpp`：
  - md seq 连续性与 Resume replay；
  - IpcClient md gap 检测；
  - TCP 断线后自动重连 + Resume；
  - epoch 变化直接返回 TradeSnapshot；
  - 同一命令 `<op>:<rid>` 重复到达时复用缓存 ack，网关只执行一次；
- `tests/e2e/sim_td_e2e_test.cpp`：`snapshot()` 可被 `DiffMergeEngine` 重建；
- 两进程冒烟：node/gw 均 4 worker 线程，杀 gw、重启 gw 后 node 自动重连，
  REST 账户/订单从新 gw 快照恢复，且能继续下单。
