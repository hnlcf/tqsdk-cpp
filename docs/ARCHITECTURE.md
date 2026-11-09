# 总体架构

> 本文描述当前实现，不描述历史方案。协议细节见
> [PROTOCOLS.md](PROTOCOLS.md)，模块文件边界见 [MODULES.md](MODULES.md)。

## 1. 设计目标

1. **数据面单一所有权**：同一份行情/交易截面只在一个核心线程上合并。
2. **真实网关可替换**：Tq 行情、CTP 交易、Sim 交易通过统一接口接入。
3. **进程隔离**：网关进程负责柜台连接，node 负责数据/API，Web UI 独立。
4. **热路径无锁**：核心 strand 串行处理 Channel 事件，避免每 tick 加锁。
5. **可恢复**：IPC 有 seq/epoch/replay；交易重启后查询对账。
6. **可观测**：readiness、metrics、catalog、holiday、soak 产物。

## 2. 进程拓扑

```text
              ┌─────────────────────────────┐
              │ 外部行情/交易源              │
              │ Tq MD / CTP TD / Sim        │
              └──────────────┬──────────────┘
                             │
              ┌──────────────▼──────────────┐
              │ tqsdk-gw                     │
              │ ---------------------------- │
              │ MarketDataGateway / Trading  │
              │ Gateway 实现                 │
              │ CTP SPI -> TaskQueue -> Asio │
              │ IPC server + replay buffer   │
              └──────────────┬──────────────┘
                             │ IPC
                    UDS/TCP, framed, seq
                             │
              ┌──────────────▼──────────────┐
              │ tqsdk-node                   │
              │ ---------------------------- │
              │ RemoteMd / RemoteTd          │
              │ TqApi + DataManager          │
              │ DiffMergeEngine              │
              │ InstrumentCatalog            │
              │ KlineStore（可选）            │
              │ HttpServer / WebSocketHub    │
              └──────────────┬──────────────┘
                             │ REST + WS
              ┌──────────────▼──────────────┐
              │ TQSDK Board / 策略 / 诊断工具       │
              │ REST/WS 唯一接口              │
              └─────────────────────────────┘
```

### 两种运行方式

- **两进程生产模式**：gw 接真实网关，node 通过 IPC 代理；
- **单进程测试/回测**：`TqApi` 直接持有 Sim/Mock/Tq 网关，不启动 gw/node。

Web 前端永远不链接 `tqsdk_core`，只通过 node REST/WS 通信。

## 3. 分层与依赖方向

```text
domain/                 纯数据与枚举
channel/                Asio 异步队列
gateway/                网关接口 + tq/ctp/sim/mock 实现
ipc/                    帧、会话、client/server、Remote 网关
data/                   DataManager / DiffMerge / KlineStore
trading/                TradingCalendar / HolidayCalendar / 价格归一化
instrument/             合约目录
tqapi/                  TqApi 门面与本地订单路径
serve/                  REST/WS/路由/快照投影
apps/                   gw_main / node_main
web/                    TQSDK Board：SolidJS + pnpm + Lightweight Charts
```

依赖必须朝向抽象：

- `gateway` 不依赖 `serve`；
- `data` 不依赖网络/HTTP；
- `serve` 可以读 `TqApi`，但不得反向写内部状态；
- UI 不依赖 core；
- `ipc` 只依赖 frame/protocol 和网关接口。

## 4. 数据所有权

| 数据 | 所有者 | 线程 |
|---|---|---|
| Quote/Order/Trade/Position/Account | node 的 `DataManager` | 单 core strand |
| KlineStream | `KlineRegistry`（DataManager 内） | core strand |
| CTP 内部查询/订单缓存 | `CtpTdGateway` | gw 的 gateway strand |
| CTP 查询窗口状态 | `CtpQueryPipeline` | gw 的 gateway strand |
| IPC replay buffer | `GatewayIpcServer` | 每连接 strand |
| WS 客户端订阅/发送缓存 | `WebSocketHub::Client` + `WsEntityFilter` | core/连接 strand |
| 图表/表格展示数据 | UI 模型 | GUI 线程 |
| 指标计算 | Python sidecar | sidecar 进程 |

关键规则：

- **DataManager 不提供跨线程写锁**；所有写发生在 core strand。
- gateway 只推事件/diff，不直接写 `DataManager`。
- `wait_update()` 清位后，本轮 changed_fields 是“自上次 wait 以来”的变化。
- `TqApi::data()` 的 const/non-const 访问都要遵守同一 strand 约束。

## 5. 线程模型

```text
gw process:
  io_context workers
    └─ gateway core strand
         ├─ CTP TaskQueue 消费
         ├─ 查询/订单/回调处理
         └─ IPC pump/control

node process:
  io_context workers
    ├─ core strand
    │    ├─ TqApi md_loop / td_loop
    │    ├─ DiffMergeEngine -> DataManager
    │    ├─ WebSocketHub push_loop / broadcast
    │    └─ HTTP route data access
    └─ per-connection WS strands（socket 读写）
         └─ 只做 I/O；数据访问 dispatch 回 core

UI process:
  GUI thread（不解析 inbound JSON）
    └─ JsonParserWorker QThread
         └─ HTTP/WS 原始字节解析、约 100ms 合并
```

CTP SPI 回调线程只做 **TaskQueue 入队**，不直接执行 Asio/DataManager 逻辑。
`Channel<T>` 的等待/唤醒基于 Asio timer，不引入额外条件变量线程。

## 6. 性能边界

- 行情热路径：`Channel -> DiffMerge -> DataManager -> WS filter/broadcast`，无锁。
- WS：服务端 `coalesce_ms` 合并短窗口变化；每客户端实体 JSON 缓存去重；
- HTTP：`/quotes` 批量行情、`/trading-snapshot` 合并交易快照；
- DiffMerge：订单字段与旧值比较后才置 dirty，避免 CTP 全量查询造成假 diff；
- K 线 SQLite writer 在独立异步路径，不阻塞行情合并；
- UI：JSON 解析在 worker；行情按 100ms 合并刷新，图表按可见窗口绘制。

性能不是通过引入更多线程解决，而是：

1. 减少不必要的序列化；
2. 减少重复广播；
3. 把阻塞 I/O/SQLite 移出 core；
4. 只在真正的边界做线程切换。

## 7. 恢复与一致性

### IPC

- 帧带 `seq`，连接带 `epoch`；
- 断线后 `Resume{epoch, next_seq}`；
- 同 epoch 且 seq 连续：replay；
- epoch 不匹配/缺口无法 replay：要求完整 resync；
- 交易快照重建前清空交易实体，避免旧订单残留。

### 交易

- TqApi 先创建本地占位订单，立即对调用者可见；
- gateway ack 只表示命令被接受，终态由 TradeEvent Diff 推进；
- 本地 ack/transport 异常会转为明确错误终态，不向 UI 抛异常；
- CTP 重启后通过持久化 order store + 查询快照对账；
- 订单状态实际字段比较后才进入 dirty。

### 交易日

`HolidayCalendar` 定时拉取非周末节假日，`TradingCalendar` 结合：

- 日盘/夜盘；
- 周末；
- 节假日前一晚无夜盘；
- 结算窗口；
- CTP readiness 的 `phase`/`expected_disconnect`。

## 8. 安全与部署边界

- 默认 REST/WS 只绑定 `127.0.0.1`；
- 当前没有 HTTP 身份认证/TLS，禁止直接暴露公网；
- CTP 账号/密码/AuthCode 不进入仓库；
- UI 与 node 之间当前是本地 HTTP/WS；未来如需远程，应加 TLS + 认证；
- `.gitignore` 应排除运行流文件、缓存、合约大文件和 artifacts。

## 9. 明确不在本层解决的问题

- 交易所完整交易日历/品种级夜盘差异（当前可通过 `TradingCalendar::Config` 覆盖，
  尚未接入远程交易日历服务）；
- 生产级 multi-account 路由；
- Windows CTP；


## 10. 相关文档

- [MODULES.md](MODULES.md)
- [FLOWS.md](FLOWS.md)
- [STATE_MACHINES.md](STATE_MACHINES.md)
- [PROTOCOLS.md](PROTOCOLS.md)
- [OPERATIONS.md](OPERATIONS.md)
