# TqSdk-Cpp 模块说明（目标 / 边界 / 约定）

> 本文档是**代码级模块参考手册**：逐模块写明它的目标、边界（负责什么、不负责什么）、
> 关键约定与生命周期。面向维护者与二次开发者。架构背景与路线图见 `docs/ROADMAP.md`，
> 项目介绍与快速上手见 `README.md`。
>
> 约定：每个模块给出【目标】【负责】【不负责/边界】【关键约定】【关键文件】。
> 若无特别说明，所有异步代码都运行在**单 io_context 线程**上（见 §2.7 线程模型）。

---

## 目录

1. 分层总览与数据流
2. 核心模块
   1. Channel<T>（异步通道）
   2. DiffMergeEngine（diff 合并引擎）
   3. DataManager / EntityStore / KlineRegistry（内存截面）
   4. TqApi（门面）
   5. Domain 领域模型
3. 网关模块
   1. MarketDataGateway / TradingGateway（抽象）
   2. TqMdGateway（天勤行情）
   3. SimTdGateway（本地撮合）
   4. MockMd/MockTd（测试网关）
   5. 网关的行情订阅 / 引用计数 / peek 闸门（TqMdGateway 专项）
4. IPC 模块（进程间通信）
   1. 帧协议 frame.h
   2. 传输通道（StreamChannel / UdsChannel / TcpChannel）
   3. 编解码抽象 codec.h
   4. 消息体协议 protocol.h
   5. IpcSession（会话：握手/心跳/看门狗）
   6. IpcClient（帧分发 + 请求响应关联）
   7. GatewayIpcServer（gw 侧桥接）
   8. RemoteMdGateway / RemoteTdGateway（node 侧代理）
5. 服务层（node 进程内 serve）
   1. Router（类型化路由）
   2. HttpServer（REST 服务）
   3. WebSocketHub（实时 diff 推送）
6. 配置与入口
   1. TqApiConfig / GwConfig / NodeConfig
   2. tqsdk-gw / tqsdk-node 两个进程
7. 工具模块
   1. spawn_logged
   2. AsyncMutex
   3. serde / log
8. 生命周期与关闭次序（跨模块）
9. 已知约束与 TODO

---

## 1. 分层总览与数据流

```text
        进程内直连模式
  ┌──────────────────────────────────────────────┐
  │  TqApi (门面)                                  │
  │   md_loop / td_loop -> DiffMergeEngine -> DataManager
  │        ▲ output()            ▲ event_output()  │
  │  ┌─────┴─────┐        ┌──────┴──────┐          │
  │  │ TqMdGateway│        │ SimTdGateway│          │
  │  └───────────┘        └─────────────┘          │
  └──────────────────────────────────────────────┘

        两进程模式（gw + node）
  tqsdk-gw                                             tqsdk-node
  ┌───────────────────────────┐        UDS/TCP        ┌──────────────────────────────┐
  │ 真实网关 (TqMd / SimTd)    │  IPC 帧  <──────────> │ IpcClient -> Remote{Md,Td}Gateway
  │   GatewayIpcServer 桥接    │  (握手/心跳/业务帧)    │   TqApi -> DiffMergeEngine     │
  │   IpcSession(Gateway 角色) │                       │   DataManager                  │
  └───────────────────────────┘                       │   HttpServer (serve 层)        │
                                                       │   IpcSession(Node 角色)        │
                                                       └──────────────────────────────┘
```

- **单一权威数据源**：只有 node 进程（或进程内直连的 TqApi）持有合并后的内存截面。
  gw 进程只做接入与协议翻译，**不持有行情截面**。
- **网关只推 diff**：所有网关把状态变更翻译成"与原协议同构的 JSON diff"塞进通道，
  合并统一在 `DiffMergeEngine` 做。
- **两种模式同一接口**：`RemoteMdGateway`/`RemoteTdGateway` 与 `TqMdGateway`/`SimTdGateway`
  都实现同一套 `MarketDataGateway`/`TradingGateway`，TqApi 无感知。

---

## 2. 核心模块

### 2.1 Channel<T>（异步通道）

- **目标**：进程内的异步队列，连接"生产者协程"与"消费者协程"，提供协程风格收发。
- **负责**：有界/无界队列；`async_send`/`async_receive`；`close()` 唤醒所有等待者；
  返回 `std::optional<T>`（关闭且空 -> `nullopt`）。
- **不负责**：跨线程调度（由调用方在正确 executor 上使用）、序列化。
- **关键约定**：
  - 容量 0 = 无界；容量 N>0 时 `async_send` 在满时**挂起**（背压）。
  - `close()` 后 `async_send` 抛 `operation_aborted`；`async_receive` 先排空再返回 `nullopt`。
  - **析构前必须保证无挂起的等待者**（否则断言）；通常由 `close()` + 让等待协程退出保证。
  - 被 `awaitable_operators`（race/cancel）取消时，`async_receive` 抛 `operation_aborted`，
    调用方需处理。
- **关键文件**：`include/tqsdk/channel/channel.h`
- **测试**：`tests/channel/*`（有界/无界/关闭/multiplex/cancel）。

### 2.2 DiffMergeEngine（diff 合并引擎）

- **目标**：把"与原协议同构的 JSON diff"合并进内存实体，并追踪脏字段。
- **负责**：quotes / trade（orders/trades/positions/accounts） / klines 的递归合并；
  按字段位标记 `changed_fields`（uint64 位图）。
- **不负责**：网络、序列化、通知（通知由 TqApi 的 update_signal 负责）。
- **关键约定**：
  - **唯一合并入口** `apply(diff)`；网关/远端不得直接改 DataManager。
  - diff 是 JSON 对象，键为实体类别，值为 `{key: fields}`。
  - 字符串占位（如未收盘的 `"-"`）不覆盖为非法数值（保持 unknown，见 `tq_md_protocol` 的 MERGE_DBL）。
  - **事务性**：一次 `apply` 内的所有实体变更视为同一批（一次 wait_update 可见）。
- **关键文件**：`include/tqsdk/data/diff_merge_engine.h`、`src/data/diff_merge_engine.cpp`、
  `include/tqsdk/data/field_table.h`
- **测试**：`tests/data/diff_merge_test.cpp`。

### 2.3 DataManager / EntityStore / KlineRegistry（内存截面）

- **目标**：承载合并后的权威数据截面，供查询与脏位遍历。
- **负责**：`EntityStore<T>`（键 -> 实体 + 脏链表，`clear_changes` 只遍历脏实体）；
  `KlineRegistry`（K 线序列，环形裁剪到 capacity，淘汰最旧 bar）；
  `DataManager` 聚合各类实体，提供 `get_*` 与 `clear_changes()`。
- **不负责**：网络、合并逻辑本身（合并见 2.2）。
- **关键约定**：
  - 单 io_context 线程读写，**不加锁**（线程模型见 §2.7）。
  - `wait_update` 语义 = "自上次 wait 以来"的变化；进入下一轮清位。
  - **serve 层同步读**：HttpServer 处理函数在构造响应时**同步**读取并立即拷贝成 JSON，
    不在 `co_await` 之间持有 data() 内部引用（下单会 co_await，读与写分离）。
- **关键文件**：`include/tqsdk/data/data_manager.h`、`entity_store.h`、
  `include/tqsdk/domain/kline.h`
- **测试**：`tests/data/entity_store_test.cpp`、`tests/data/kline_*`。

### 2.4 TqApi（门面）

- **目标**：面向用户策略的统一门面：启动网关、驱动后台 loop、合并数据、提供查询/下单。
- **负责**：
  - 构造时持有一个 `MarketDataGateway` 与一个 `TradingGateway`（可为 nullptr）。
  - `start()` 启动网关并 co_spawn `md_loop`/`td_loop`（消费网关 output，喂 `DiffMergeEngine`）。
  - `wait_update()`：等待自上次以来的变化（基于 update_signal），进入下一轮清位。
  - `get_quote/get_kline/get_account/get_order/...`：读 DataManager。
  - `insert_order/cancel_order`：经 OrderRouter 转给 TradingGateway。
  - `close()`：关闭网关、唤醒所有挂起 wait_update、清理 hook。
- **不负责**：具体协议、IPC 细节（都由网关抽象承担）。
- **关键约定**：
  - 生命周期：`start()` 之后 `io.run()` 驱动；`close()` 幂等。
  - `insert_order` 本地占位单**立即可见**，ack 失败即错误终态。
  - md_loop/td_loop 是唯一把网关事件写入 DataManager 的地方。
- **关键文件**：`include/tqsdk/tqapi.h`、`src/api/tq_api.cpp`
- **测试**：`tests/e2e/mock_e2e_test.cpp`、`mock_e2e_edges_test.cpp`、`sim_td_e2e_test.cpp`。

### 2.5 Domain 领域模型

- **目标**：Quote/Order/Trade/Position/Account/Kline/Sentinel 等数据结构与枚举。
- **负责**：数据字段定义、枚举（Direction/Offset/PriceType/OrderStatus…）、`changed_fields`。
- **不负责**：合并/序列化（由 data/ipc 层处理）。
- **关键约定**：字段名与 TqSdk Python 版保持同构，便于 diff 直接映射；
  时间戳统一用 `TimeStamp`（纳秒 int64）。
- **关键文件**：`include/tqsdk/domain/*.h`、`include/tqsdk/types.h`

---

## 3. 网关模块

### 3.1 MarketDataGateway / TradingGateway（抽象）

- **目标**：把行情/交易解耦成可替换接口；同一接口既有进程内实现也可远程代理。
- **MarketDataGateway 负责**：
  - `start()/stop()`：生命周期，`stop()` 关闭 `output()` 使 md_loop 以 nullopt 退出。
  - `subscribe(symbols)`：订阅合约行情。
  - `subscribe_kline(symbol, duration_ns, view_width)`：订阅 K 线（默认空实现，网关按需覆写）。
  - `release(symbols, klines)`：**释放某条连接的订阅**（引用计数退订，默认空实现）。
  - `output()`：`Channel<MarketDataEvent>&`，把 diff/error/disconnected 推给上层。
- **TradingGateway 负责**：
  - `start()/stop()`，`insert_order(req, rid)`、`cancel_order(req)` 返回 `CommandAck`。
  - `event_output()`：`Channel<TradeEvent>&`，推 trade diff / ack / error / disconnected。
- **不负责**：合并、解析（网关只做"协议 <-> 与协议同构的 diff"翻译）。
- **关键约定**：网关**只推 diff，不落库**；状态变更一律经通道交给 TqApi 合并。
- **关键文件**：`include/tqsdk/gateway/market_data_gateway.h`、`trading_gateway.h`

### 3.2 TqMdGateway（天勤行情）

- **目标**：接入天勤免费行情 WebSocket（TLS + Beast），翻译成内存 diff 推送。
- **负责**：TLS/WS 握手（含 permessage-deflate 与 Host 头）、`subscribe_quote` 盘口、
  `set_chart` K 线序列、peek_message 流控、重连补发订阅、字段翻译
  （amount->turnover、10 档->5 档裁到 5 档，见 `tq_md_protocol`）。
- **不负责**：交易、合并。
- **关键约定**：
  - 帧 `rtn_data.data[]` 用 `normalize_rtn_data` 递归合并成"单条引擎可消费 diff"。
  - `chart_id` 32 位十六进制，同一 `(symbol,duration)` 复用稳定 chart_id。
  - 断线先投递 `Disconnected` 事件，再关闭通道；阶段4 暂不自动重连（TODO）。
  - `stop()` 直接关底层 socket，立即中断挂起读写（避免优雅退出卡死）。
- **关键文件**：`include/tqsdk/gateway/tq/tq_md_gateway.h`、`src/gateway/tq/tq_md_gateway.cpp`、
  `tq_md_protocol.{h,cpp}`
- **测试**：`tests/gateway/tq_md_protocol_test.cpp`（离线协议）、`tests/e2e/tq_md_network_test.cpp`（联网，默认跳过）。

### 3.3 SimTdGateway（本地撮合）

- **目标**：`TradingGateway` 的本地模拟实现，行为与真实柜台同构。
- **负责**：市价/限价撮合（到价成交、部分成交、撤单）、保证金/手续费/风控度模型；
  一次撮合产生的 order/trade/position/account 变更放进**同一个 trade diff**（事务性）。
- **不负责**：行情（撮合价来自内部 `last_price_` 表，由 `on_tick` 显式驱动，非抽象的一部分）。
- **关键约定**：所有状态推进走 `event_output()` 的 diff；模拟器不碰 DataManager。
- **关键文件**：`include/tqsdk/gateway/sim/sim_td_gateway.h`、`src/gateway/sim/sim_td_gateway.cpp`
- **测试**：`tests/e2e/sim_td_e2e_test.cpp`。

### 3.4 MockMd / MockTd（测试网关）

- **目标**：脚本化网关，测试中 `inject()` 注入 diff，走与真实网关完全相同的通道。
- **关键文件**：`include/tqsdk/gateway/mock/*`
- **测试**：`tests/e2e/mock_e2e_test.cpp` 等。

### 3.5 行情订阅 / 引用计数 / peek 闸门（TqMdGateway 专项）

> 这是两进程多 node 共享同一 gw 网关时的关键设计，容易被忽视。

- **背景**：`TqMdGateway` 在 gw 进程内被**多个 node 连接共享**。天勤行情协议**不支持软退订**
  （无法可靠地"取消订阅某个 symbol"）。若不处理，node 断联后行情会持续入队到无人消费的通道，内存无限增长。
- **三条配合机制**：
  1. **引用计数**：`quote_ref_[symbol]` 与 `chart_ref_[chart_id]` 统计"尚被多少连接需要"。
     `subscribe` 使 quote_ref+1；`release` 使 -1（减到 0 才真正从 `pending_subs_`/`pending_klines_` 移除）。
  2. **投递前过滤**：`deliver_rtn_data` 里 `filter_merged()` 按 ref 判活，
     **入队前丢弃**无人需要的 quotes/klines（K 线用 `(symbol,duration)->chart_id` 映射 + 帧内
     `charts.<id>.state` 校正后判活）。
  3. **peek 闸门**：`read_loop` 在**无任何订阅**时不发 `peek_message`，挂在 `peek_wakeup_` 定时器上；
     订阅到来时 `cancel()` 唤醒。无 node -> 不 peek -> 服务器停推 -> 零入队。
- **计数对称性（易错点）**：
  - `GatewayIpcServer::handle_subscribe` 用 `symbol_use_` 做**连接内**计数：
    显式 symbol +1，kline 依赖再 +1；只有 0->1 才 `md_->subscribe()`。
  - `subscribe_kline` **不再**自行增加 `quote_ref_`（合约行情由上面的统一 subscribe 驱动），
    否则 `release` 永远减不到 0。
  - `SubscribeBody.release=true` 按同样规则 -1；归零才 `md_->release()`。
    断联时 `GatewayIpcServer::close()` 一次性释放剩余订阅。
  - 同一连接重复订阅同一 kline（`sym|dur`）只计一次，避免 chart_ref 虚高。
- **兜底**：`ch_` 设容量上界 `kOutputCapacity=8192`，`async_send` 满时阻塞形成背压。
- **关键文件**：`src/gateway/tq/tq_md_gateway.cpp`（subscribe/release/filter_merged/read_loop）、
  `src/ipc/gateway_server.cpp`（handle_subscribe/close）。
- **测试**：`tests/ipc/gateway_server_test.cpp::DisconnectReleasesSubscriptions`、
  `ReleaseFrameUpdatesConnectionRefCounts`。

---

## 4. IPC 模块（进程间通信）

### 4.1 帧协议 frame.h

- **目标**：定义 gw<->node 的二进制帧格式与帧类型表。
- **帧格式（固定）**：`[4B BE length][1B type][1B encoding][payload]`，
  其中 `length = 2 + payload.size()`（含 type+encoding 两字节）。`kMaxPayload = 2<<20`（2MB）。
- **FrameType**：Hello/HelloAck/Heartbeat/HeartbeatAck/Goodbye/Subscribe/SubscribeAck/
  MarketDataPush/TradePush/Command/CommandAck/GatewayEvent（+ 预留）。
- **关键约定**：帧头格式是**跨语言契约**，不得随意更改。
- **关键文件**：`include/tqsdk/ipc/frame.h`、`ipc_channle.h`（`IIpcChannel` 抽象、`ReceivedFrame`）。

### 4.2 传输通道（StreamChannel / UdsChannel / TcpChannel）

- **目标**：在 socket 上做帧编解码，对上暴露 `async_send/async_recv`。
- **`StreamChannel<Socket>`（CRTP 模板基类）负责**：帧头编解、scatter/gather 读写、
  EOF/connection_reset 返回 `nullopt`、非法/超大帧抛 `IpcError`、`close()` 幂等。
- **`UdsChannel` / `TcpChannel`**：分别基于 `asio::local::stream_protocol` 与 `asio::ip::tcp`；
  各提供静态 `connect(io, ...)`（TCP 用同步 resolver+connect）。
- **不负责**：握手/心跳/业务语义（那是 IpcSession）。
- **关键约定**：`async_recv` 遇到对端关闭返回 `std::nullopt`（正常 EOF），不是异常。
- **关键文件**：`include/tqsdk/ipc/stream_channel.h`、`uds_channel.h`、`tcp_channel.h` 及对应 `.cpp`
- **测试**：`tests/ipc/uds_channel_test.cpp`、`tcp_channel_test.cpp`。

### 4.3 编解码抽象 codec.h

- **目标**：把"消息体 <-> 字节"与具体线格式解耦，预留 MessagePack。
- **负责**：`ICodec`（`encoding()`、`encode(json)->bytes`、`decode(span)->json`）；
  `JsonCodec`（dump/parse）、`MsgpackCodec`（to_msgpack/from_msgpack）；
  `codec_for(FrameEncoding)` 单例；`kDefaultEncoding`、`default_codec()`。
- **关键约定**：消息体**只与 `nlohmann::json` DOM 打交道**，绝不耦合具体线格式；
  帧头 `encoding` 字节选择 codec。
- **关键文件**：`include/tqsdk/ipc/codec.h`

### 4.4 消息体协议 protocol.h

- **目标**：为每个 FrameType 定义消息体 schema 及序列化。
- **负责**：`IpcRole`、`kIpcProtocolVersion`；各 body（Hello/HelloAck/Subscribe/SubscribeAck/
  MarketDataPush/TradePush/Command/CommandAck/Heartbeat/Goodbye/Opaque 等）的 `to_json/from_json`；
  `encode_body`/`decode_body<Body>`/`decode_as<T>`/`encode_frame<T>` 辅助。
- **关键约定**：body 间通过 `FrameBody` variant 与 `BodyFor<T>` trait 关联；
  编解码始终经 `codec_for(frame.encoding)`，不硬编码 JSON。
  `SubscribeBody.release=false` 表示订阅（默认，旧帧兼容），`true` 表示退订
  symbols/klines；用于把 WebSocket 下游引用计数归零事件转发给远端 gw。
- **关键文件**：`include/tqsdk/ipc/protocol.h`
- **测试**：`tests/ipc/protocol_test.cpp`、`protocol_e2e_test.cpp`。

### 4.5 IpcSession（会话：握手/心跳/看门狗）

- **目标**：在一条 `IIpcChannel` 上提供"有条理的全双工连接"。
- **负责（控制面）**：
  1. **握手状态机**：双方启动即互发 Hello；收到对方 Hello 回 HelloAck；见 HelloAck(ok)
     或对方 Hello 即 Ready；握手超时失败关闭。角色必须不同（Gateway vs Node）。
  2. **心跳**：周期发 Heartbeat；**收到任何帧都刷新看门狗**；超过 heartbeat_timeout 无帧判定断线关闭。
  3. **收发分离**：内部读循环 `async_recv -> dispatch_frame`；业务帧投递到 `incoming()`。
  4. **优雅关闭**：可发 Goodbye + close；幂等。
- **不负责**：seq 编号承载（body 字段由上层决定）、业务语义。
- **关键约定**：
  - `start()` 返回时握手已成功（失败抛异常）；读/心跳/看门狗在后台协程运行。
  - 出站写经 `AsyncMutex` 串行化（socket 不允许并发 async_write）。
  - `close()` 幂等；`incoming()` 关闭后 `async_receive` 返回 nullopt。
  - 所有方法在 io_context 线程调用。
- **关键文件**：`include/tqsdk/ipc/ipc_session.h`、`src/ipc/ipc_session.cpp`
- **测试**：`tests/ipc/ipc_session_test.cpp`。

### 4.6 IpcClient（帧分发 + 请求响应关联）

- **目标**：跑在一条 session 之上的"帧分发器 + 请求/响应关联器"。
- **负责**：**单一后台读循环**消费所有入站业务帧，按 FrameType 路由给已注册的
  md/td 处理器（避免多消费者抢同一 `incoming()`）；提供 `send_command_and_wait`
  （按 rid 关联 CommandAck，带超时）。
- **不负责**：业务翻译（处理器由 Remote{Md,Td}Gateway 注册）。
- **关键约定**：`start()` 前先 `set_md_handler`/`set_td_handler`；`send_command_and_wait`
  超时/关闭抛 `IpcError`；`stop()` 发 Goodbye 并唤醒所有 pending 请求。
- **关键文件**：`include/tqsdk/ipc/ipc_client.h`、`src/ipc/ipc_client.cpp`

### 4.7 GatewayIpcServer（gw 侧桥接）

- **目标**：把**真实网关**桥接成一条 IPC 连接（生产端）。
- **负责**：`md_pump`/`td_pump` 把网关 output 编码成 MarketDataPush/TradePush/GatewayEvent 上推；
  `control_loop` 消费 Subscribe/Command 驱动真实网关并回 ack；
  **记录本连接订阅**（`symbol_use_` 计数 = 显式 symbols + kline 依赖），
  处理 `SubscribeBody.release=true`，并在 `close()` 时向 md 网关释放剩余订阅。
- **不负责**：合并（node 侧 TqApi 做）。
- **关键约定**：
  - 每个后台循环持 `shared_from_this()`，循环退出前本对象不析构。
  - md/td 各用一个停止 timer（`md_stop_timer_`/`td_stop_timer_`），避免互相取消挂起等待。
  - `close()` 幂等：cancel 停止 timer、关 session、`co_spawn` release 订阅（按值捕获快照）。
  - 同一 symbol 被显式订阅且被 K 线依赖时计数为 2；只退订行情不会提前释放合约行情。
  - 同连接重复订阅同一 kline 不重复计数（按 `sym|dur` 去重）。
  - td 的 Ack **不上推**（已作为 CommandAck 同步返回）。
- **关键文件**：`include/tqsdk/ipc/gateway_server.h`、`src/ipc/gateway_server.cpp`
- **测试**：`tests/ipc/gateway_server_test.cpp`。

### 4.8 RemoteMdGateway / RemoteTdGateway（node 侧代理）

- **目标**：把远端 gw 的网关"代理"成本地 `MarketDataGateway`/`TradingGateway`。
- **负责**：`subscribe/subscribe_kline` -> 发 Subscribe 帧（`release=false`）；
  `release` -> 发 Subscribe 帧（`release=true`）；`insert_order/cancel_order`
  -> 经 IpcClient 发 Command 并等待 CommandAck；把远端推送解码成 `MarketDataEvent`/
  `TradeEvent` 推入本地通道。
- **不负责**：连接管理（由 IpcClient/session）。
- **关键约定**：与非远程网关同接口，TqApi 无感知；拥有模式可共享 `shared_ptr<IpcClient>`
  （同一连接承载 md+td）。
- **关键文件**：`include/tqsdk/ipc/remote_md_gateway.h`、`remote_td_gateway.h`、对应 `.cpp`
- **测试**：`tests/ipc/gateway_server_test.cpp`（含真实 UDS 往返）、`ipc_proxy_test.cpp`。

---

## 5. 服务层（node 进程内 serve）

### 5.1 Router（类型化路由）

- **目标**：把"解析路径/取 body/反序列化参数/组装响应"从业务剥离，handler 按**参数类型**自动提取。
- **负责**：声明式注册（`get/post/put/del`、表形式 `add`）；路径参数（`{name}`）、
  `Query<T>`（查询串）、其它类型（请求体 JSON）；返回值 json/Response/void -> 200/自定义/204；
  `HttpError` -> 对应状态码。
- **关键约定**：handler 必须返回 `asio::awaitable<T>`（与全项目协程风格一致）；
  全程零新依赖（基于 Beast）。
- **关键文件**：`include/tqsdk/serve/router.h`

### 5.2 HttpServer（REST 服务）

- **目标**：node 进程对外的只读 + 下单 REST 接口，并在同端口承载 WebSocket 升级。
- **负责**：accept 循环、单请求会话、路由分发；路由见 `http_server.h` 头注释
  （/healthz、/quote/{symbol}、/kline/{symbol}/{duration_ns}、/account、/orders、/trades、
  /positions、POST /order、DELETE /order/{id}）。检测到 `Upgrade: websocket` 且路径匹配
  `[serve.ws].path` 时，把 socket 交给 `WebSocketHub::accept()`；普通 GET /stream 返回 426。
- **不负责**：IPC（serve 直接读 core 内存截面，同进程同线程）。
- **关键约定**：
  - **同步读**：构造响应时同步读 DataManager 并立即拷贝成 JSON，不在 co_await 间持有内部引用。
  - 请求体上限 1MB；错误以 4xx/5xx + `{"error":...}` 返回，不抛到顶层。
  - GET /quote 首次访问触发订阅；建议 node 启动按配置预订阅（否则空占位）。
- **关键文件**：`include/tqsdk/serve/http_server.h`、`src/serve/http_server.cpp`
- **测试**：`tests/serve/ws_stream_test.cpp`（同时覆盖 HTTP 端口上的真实 WS 往返）

### 5.3 WebSocketHub（实时 diff 推送）

- **目标**：把 core 数据截面的本轮变化按客户端订阅过滤后，通过 `/stream` 增量推送。
- **负责**：
  - HTTP Upgrade（Beast）后接管 socket；每连接 reader + writer 两条协程。
  - **writer 独占写**：JSON 消息与服务端 WebSocket ping 都由 writer 发出；
    每连接有界待写队列（默认 1024），满时丢最旧消息，慢客户端不拖垮进程。
  - **单一 `wait_update` 消费者**：`push_loop` 是 serve 层唯一等待 TqApi 更新的协程，
    `build_diff()` 读取 `changed_fields` 并生成全局 diff，`filter_diff()` 按连接过滤后广播。
  - **订阅引用计数**：`quote_refs_` / `kline_refs_` 聚合多客户端订阅；0->1 调
    `TqApi::subscribe_quotes/subscribe_kline`，1->0 调 `TqApi::release`；断连时统一回收。
  - `welcome` / `subscribed` / `snapshot` / `diff` / `pong` / `error` 消息协议。
- **不负责**：合并、下单（仍经 HTTP 路由）。
- **关键约定**：
  - quote diff 只含变化字段；K 线 `{"reset":bool,"bars":[...]}`，reset=false 时仅最后一根。
  - 交易类 topic（account/orders/trades/positions）首次订阅未显式给出 `topics` 时默认全开。
  - 单 io_context 线程，订阅表/队列不加锁。
  - 后台 `push_loop` 与客户端协程都用 `co_spawn_logged`，异常不静默吞掉。
- **关键文件**：`include/tqsdk/serve/ws_stream.h`、`src/serve/ws_stream.cpp`、
  `include/tqsdk/serve/json_convert.h`、`src/serve/json_convert.cpp`
- **测试**：`tests/serve/ws_stream_test.cpp`（快照/部分 diff/K 线 reset/退订 release/ping/config）。

---

## 6. 配置与入口

### 6.1 TqApiConfig / GwConfig / NodeConfig

- **目标**：统一配置模型（TOML），屏蔽"进程内直连 / 远程代理"的差异。
- **负责**：`load/parse_tqapi_config`、`parse/load_gw_config`、`parse/load_node_config`；
  `make_tqapi(ctx, cfg)` 协程化构造 TqApi（远程模式需先建连握手）。
- **关键约定**：
  - 每个网关独立选 kind：`MdKind{Tq,Remote,Mock,None}`、`TdKind{Sim,Remote,Mock,None}`。
  - 远程模式 md/td 共享同一条 IpcSession（一个 gw 连接同时承载行情与交易）。
  - 两端 Hello 角色必须不同（连 gw 用 Node，策略连 node.serve 用 Gateway）。
  - 缺必填项/语法错误 fail-fast 抛 `std::runtime_error`。
- **关键文件**：`include/tqsdk/config/tqapi_factory.h`、`src/config/tqapi_factory.cpp`、
  `config/gw.toml`、`config/node.toml`
- **测试**：`tests/config/tqapi_factory_test.cpp`、`tests/serve/ws_stream_test.cpp`
  （`[serve.ws]` 解析）。

### 6.2 tqsdk-gw / tqsdk-node 两个进程

- **tqsdk-gw（接入进程）**：监听 UDS/TCP 端点；node 连上后为其提供真实网关能力
  （`GatewayIpcServer` 桥接）。支持多 node 同时连接（共享同一 md 网关，见 §3.5）。
  收到 SIGINT/SIGTERM 优雅退出（`md->stop()`/`td->stop()` + 关闭所有连接）。
- **tqsdk-node（数据服务进程）**：作为 `IpcRole::Node` 连 gw，进程内 `TqApi` 合并成权威截面，
  再经 `HttpServer` 对外暴露 REST，并在同一端口用 `WebSocketHub` 提供 `/stream` 增量推送。
- **关键文件**：`apps/gw_main.cpp`、`apps/node_main.cpp`
- **运维脚本**：`scripts/smoke_two_process.sh`（两进程冒烟）、`debug_smoke.sh`、
  `probe_md.py`/`probe_kline.py`（协议探测）。

---

## 7. 工具模块

### 7.1 spawn_logged

- **目标**：`co_spawn` + 异常兜底日志的包装，避免后台协程异常静默丢失。
- **约定**：后台长生命周期协程一律用 `co_spawn_logged(ex, fn, "name")`。
- **关键文件**：`include/tqsdk/util/spawn_logged.h`

### 7.2 AsyncMutex

- **目标**：协程友好的异步互斥，给"必须串行化的异步临界区"加锁（如 socket 写、session 写）。
- **约定**：`auto guard = co_await AsyncMutex::acquire(m);` RAII 释放；
  用于单线程下也需保证"先登记后发送"时序的场合。
- **关键文件**：`include/tqsdk/util/async_mutex.h`

### 7.3 serde / log

- **serde**：枚举 <-> 字符串（magic_enum + `DEFINE_ENUM_JSON_SERIALIZE` 宏）。
- **log**：spdlog 分级/着色/文件轮转；测试 `main()` 统一初始化。
- **关键文件**：`include/tqsdk/util/serde.h`、`util/log.h`、`src/util/log.cpp`

---

## 8. 生命周期与关闭次序（跨模块）

**进程内直连**：
```
构造网关 -> TqApi.start()（启动 md_loop/td_loop）
  -> io.run()
  -> TqApi.close()：关闭网关 output -> loop 以 nullopt 退出 -> 唤醒 wait_update
  -> io 排空 -> 析构（析构前确保无挂起等待者）
```

**gw 进程**：
```
监听端点 -> accept node -> IpcSession.start()(握手) -> GatewayIpcServer.start()
  -> 信号(SIGINT/TERM)：md->stop()/td->stop() + 所有 srv->close()
  -> srv.close()：cancel 停止 timer + session.close() + co_spawn release 订阅
  -> io 排空 -> 退出
```

**node 进程**：
```
连 gw -> IpcClient(start) -> Remote{Md,Td}Gateway -> TqApi.start()
       -> HttpServer.listen() -> io.run()
  -> 信号：HttpServer.stop() + TqApi.close() + IpcClient.stop()(发 Goodbye)
  -> io 排空 -> 退出
```

**关键不变量**：
- 关闭任何一个子系统前，先让阻塞在其上的协程收到信号（cancel timer / close channel）。
- 网关 output 关闭 = 对应 loop 正常退出；不等于关整条连接。
- 析构对象前，其协程必须已退出（Channel 析构有挂起等待者会断言）。

---

## 9. 已知约束与 TODO

- **线程模型**：当前全链路单 io_context 线程，内部状态不加锁。多线程/strand 未验证（roadmap 阶段8）。
- **自动重连**：`TqMdGateway` 断线暂不自动重连（投递 Disconnected 后关闭）；重连补发机制已有雏形（pending_subs_/pending_klines_）。
- **seq / gap 检测**：帧 body 尚无单调 seq；断线补发/重放未实现（roadmap 4.6）。
- **CTP 网关**：未实现（roadmap 阶段4）。
- **共享内存快通道**：未实现（性能阶段可选项）。
- **WebSocket 与 wait_update**：`HttpServer` 内的 `WebSocketHub::push_loop` 是 serve 层
  唯一的 `wait_update()` 消费者；同一进程内不要再启动第二个等待者（会破坏清位语义）。
- **WebSocket 退订与直连订阅**：hub 只对自己经 `subscribe_quotes` 建立的引用计数负责；
  若同进程其它代码先用 `get_quote()` 订阅、随后经 WS 退订，网关可能提前失去该订阅。
  生产环境建议行情订阅统一走 hub（或 node 启动预订阅后不再释放）。
- **测试**：2 个联网行情测试默认跳过（无网络环境安全）。
