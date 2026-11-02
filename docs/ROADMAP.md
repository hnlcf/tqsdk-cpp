# TqSdk-Cpp 项目规划与路线图

> 目的：梳理当前进度、测试缺口、下一阶段目标、两进程拆分（接入 / 数据+服务）架构重设计、抽象调整、配置项与性能优化，形成清晰可执行路线。供评审。
>
> **模块级参考（目标/边界/约定）见 [`docs/MODULES.md`](./MODULES.md)。**

---

## 进度更新（相对下方原始规划）

下方各章节是最初的规划文本，部分内容**已经落地**。截至当前，阶段 5（IPC 协议端到端）与阶段 6 的
相当一部分、阶段 7 的基础 REST 服务已完成：

- ✅ **阶段 5 IPC 协议端到端**：`ipc/stream_channel`（UDS/TCP 共用帧编解码）、`ipc/codec`（JSON/MessagePack 抽象）、
  `ipc/protocol`（各 FrameType body schema）、`ipc/ipc_session`（握手/心跳/看门狗）、`ipc/ipc_client`（帧分发 +
  rid 关联）、`ipc/gateway_server`（gw 侧桥接）、`ipc/remote_{md,td}_gateway`（node 侧代理）。含完整单测与 e2e。
- ✅ **阶段 6 两进程骨架**：`apps/gw_main.cpp`(tqsdk-gw) / `apps/node_main.cpp`(tqsdk-node)；`config/tqapi_factory`
  统一配置与 `make_tqapi`。多 node 共享 gw 网关 + 引用计数退订 + peek 闸门（见 MODULES §3.5）。
- ✅ **阶段 7 基础 REST**：`serve/router`（类型化路由）、`serve/http_server`（quote/kline/account/orders/trades/
  positions/下单/撤单）。
- ⬜ 未完成：CTP（阶段4）、seq/gap 检测与断线补发（4.6）、共享内存快通道、多线程/strand、WS 推送、soak。

> 下方 4~10 章保留为**设计意图与原始规划**，与已实现代码若有出入，以 `docs/MODULES.md` 为准。

---


## 目录

1. 当前进度总览
2. 已实现能力清单
3. 测试覆盖现状与缺口
4. 目标架构：接入进程 + 数据服务进程（两进程）
5. 现有抽象需要做的改造
6. 阶段路线图
7. 配置项设计
8. 性能优化方向
9. 风险与权衡
10. 附录：文件清单与关键决策

---

## 1. 当前进度总览

项目在一套 **单进程、单 io_context** 的 Asio 异步架构上，已经打通了从行情接入 -> diff 合并 -> 内存数据截面 -> 用户策略读取/下单的完整闭环，并具备 IPC（UDS）通信底座与模拟交易网关。

### 已完成的能力分层

| 层 | 组件 | 状态 |
|---|---|---|
| 异步执行引擎 | Boost.Asio（io_context + awaitable 协程 + Channel） | 完整 |
| 数据流抽象 | `Channel<T>`（有界/无界、multiplex、close 语义） | 完整且充分测试 |
| 领域模型 | Quote/Order/Trade/Position/Account/Kline + changed_fields 位标记 | 完整 |
| Diff 合并引擎 | `DiffMergeEngine`（quotes/trade/klines 递归合并，脏位追踪） | 完整 |
| 内存数据截面 | `DataManager` + `EntityStore` + `KlineRegistry` | 完整 |
| 门面 | `TqApi`（wait_update / get_* / insert_order / cancel_order） | 核心可用 |
| 行情网关 | `TqMdGateway`（天勤 WebSocket，盘口 + K线 set_chart） | 真实连接可用 |
| 交易网关（模拟） | `SimTdGateway`（本地撮合、资金/持仓模型） | 可用 |
| 交易网关（mock） | `MockMdGateway` / `MockTdGateway`（测试用） | 完整 |
| IPC 底座 | `IIpcChannel` + `UdsChannel` + `Frame` 协议定义 | 通道可用 |
| 消息序列化 | `msg.h`（aid 驱动的 JSON 消息）+ serde | 完整 |
| 日志 | `util/log`（spdlog，分级/着色/文件轮转） | 完整 |
| 构建 | CMake + vcpkg + presets（clang/mold/ASan） | 完整 |

### 关键现状判断

- **抽象边界已"为拆分预留"但尚未落地**：`MarketDataGateway` / `TradingGateway` 已把行情/交易解耦，`ipc/frame.h` 里甚至已完整设计了拆分所需的帧类型表（Hello/Subscribe/Snapshot/Diff/Command/...），但**目前没有任何代码真正使用这套 IPC 协议做进程间通信**——`UdsChannel` 只有单元测试，没有 gateway / core 两个角色的实际实现。
- **数据模型天然适合拆分**：`changed_fields` 位标记 + diff 合并 + 快照/增量语义，正是跨进程 "Snapshot + Diff" 推送所需要的形态。
- **单线程假设**：`TqApi` 明确"阶段 0~3 使用单线程 io_context，内部状态不做同步保护"。这是当前最大的隐含约束；拆分多进程后数据面天然隔离，但进程内仍需明确线程模型。

---

## 2. 已实现能力清单

### 2.1 行情侧（上-行情）
- 天勤免费行情 WebSocket 接入（TLS + Beast），匿名会话。
- `subscribe_quote` 盘口订阅，字段翻译（amount->turnover，10 档->5 档）。
- `set_chart` K 线序列订阅：`chart_id` 32 位十六进制、`view_width`、首帧全量/增量单根。
- peek_message 流控循环（读一次发一次 peek）。
- 重连后补发订阅（pending_subs_ / pending_klines_）。

### 2.2 数据侧（中）
- 递归 diff 合并到内存截面，按字段位标记脏。
- `EntityStore` 脏链表优化（clear_changes 只遍历脏实体，与总量无关）。
- `KlineStream` 裁剪语义（capacity 裁剪最旧 bar）。
- `wait_update` 语义对齐 TqSdk（"自上次 wait 以来"的变化，进入下一轮清位）。

### 2.3 交易侧
- `SimTdGateway`：本地撮合（限价穿越/市价/部分成交/撤单）、保证金/手续费/风控度模型。
- 交易状态推进一律走 trade diff（与真实柜台同构，模拟器不碰 DataManager）。
- `insert_order` 本地占位单立即可见，ack 失败立即错误终态。

### 2.4 基础设施
- Channel 家族：basic/bounded/multiplex/close 全覆盖测试。
- UDS IPC 通道：帧编解码、大 payload(1MB)、对端关闭、非法帧、幂等 close、双向并发。
- 日志分级与文件轮转；测试 `main()` 统一初始化。
- 构建：`TQSDK_BUILD_EXAMPLES`、clang++、mold、ASan 全绿；128 测试通过（含 2 个联网）。

### 2.5 测试规模快照

| 文件 | 用例数 | 重点 |
|---|---:|---|
| msg_test.cpp | 23 | 消息序列化/反序列化/字段校验 |
| diff_merge_test.cpp | 14 | diff 合并各分支 |
| serde_test.cpp | 13 | 序列化工具 |
| uds_channel_test.cpp | 11 | IPC 通道 |
| tq_md_protocol_test.cpp | 11 | 天勤协议编解码 |
| sim_td_e2e_test.cpp | 10 | 模拟撮合端到端 |
| mock_e2e_edges_test.cpp | 10 | 门面边界场景 |
| channel_close_test.cpp | 7 | 通道关闭 |
| entity_store_test.cpp | 6 | 脏链表 |
| mock_e2e_test.cpp | 5 | mock 端到端 |
| 其余 | ~18 | channel/log/spawn/data_manager |
| **合计** | **~128** | |

---

## 3. 测试覆盖现状与缺口

### 3.1 覆盖良好
- Channel 全部语义（有界/无界/关闭/multiplex）。
- Diff 合并 + 实体脏位。
- UDS 通道协议健壮性（含恶意输入、1MB payload、并发）。
- 天勤协议编解码（离线）。
- 模拟撮合主路径。

### 3.2 未被充分测试的区域（缺口）

> 按优先级排序，★ 越多越关键。

| 区域 | 缺口描述 | 优先级 |
|---|---|---|
| **CTP 网关** | 完全无实现、无测试；`third_party/ctp` 有 SDK 头/so，但 `src/gateway/ctp/*` 不存在。TaskQueue/SPI 桥接、断线重连、报单回报时序全空白。 | ★★★★★ |
| **多线程 io_context** | 当前所有测试单线程。strand 串行化、网关内部状态并发保护、Channel 跨线程投递均未验证。 | ★★★★★ |
| **重连与容错** | `TqMdGateway` 重连逻辑（补发订阅、chart_id 复用、peek 循环恢复）无针对性测试（联网测试仅覆盖"一次成功订阅"）。 | ★★★★☆ |
| **wait_update 并发语义** | 多个协程同时 wait_update、close 时挂起者唤醒、清位时序在并发下是否正确，未测。 | ★★★★☆ |
| **KlineStream 裁剪/边界** | capacity 裁剪最旧 bar、Appended 位、首帧全量覆盖、空序列读取，仅有分散断言，无系统测试。 | ★★★☆☆ |
| **insert_order 竞态** | 占位单在 ack 前可见、ack 失败转错误终态、网关无响应超时，边界未测。 | ★★★☆☆ |
| **交易资金模型边界** | 爆仓、保证金不足拒单、部分平仓、多空双向持仓、手续费边界，模拟器覆盖不足。 | ★★★☆☆ |
| **序列化健壮性** | 异常/超大/深层嵌套 JSON、MessagePack 路径（当前多走 JSON），未测。 | ★★☆☆☆ |
| **IPC 端到端角色** | `FrameType` 定义的 Subscribe/Snapshot/Diff/Command 等协议**无任何收发方实现**，只有裸通道。 | ★★★★☆（拆分前置） |
| **资源泄漏/长稳** | 无 soak test（长时间跑、订阅大量合约、内存增长）。 | ★★☆☆☆ |
| **跨平台** | 仅 Linux；Windows（CTP 主要运行于 Win）未验证。 | ★★☆☆☆ |

### 3.3 测试策略建议
3. **重连用可控的"假服务器"**：在测试内起本地 WebSocket server（Beast）模拟天勤，注入断线/乱序/慢速，覆盖重连。
4. **soak test 作为 CI 夜间任务**，不进常规快速测试。

---

## 4. 目标架构：接入进程 + 数据服务进程（两进程）

> 决策：**只拆两个进程**。`gateway` 独立成进程（因为 CTP 必须同进程、且是"外网/柜台"边界）；
> `core`（数据状态）与 `serve`（对外接口）**合并进同一进程**。下层 HTTP/WS 只是进程内的若干模块，
> **不单独拆进程**，但在代码上以目录/接口划清边界（core 数据层 vs serve 服务层）。
> 这样既拿到了"网关隔离、崩溃不互相拖垮"的收益，又避免了 serve 单独进程带来的运维与 IPC 复杂度。

### 4.1 两段式

```text
┌──────────────────────────────┐
│  接入进程 (gateway)           │  针对 tq 行情 与 CTP 柜台
│  · TqMdGateway / CtpMdGateway │  负责：网络接入、协议翻译、重连
│  · CtpTdGateway (交易)        │  输出：MarketDataPush / TradePush (IPC)
└───────────────┬──────────────┘
                │ IPC (UDS)  上行：原始 push；下行：command/subscribe
┌───────────────▼──────────────────────────────────────┐
│  数据服务进程 (core + serve)                          │
│  ┌────────────────────────┐  ┌─────────────────────┐ │
│  │ core 数据层（代码边界）  │  │ serve 服务层（代码边界）│ │
│  │ · DiffMergeEngine       │  │ · HTTP / WebSocket   │ │
│  │ · DataManager           │◀─┤ · 订阅管理/查询/下单   │ │
│  │ · OrderRouter           │  │ · 鉴权/限流/CORS      │ │
│  └────────────────────────┘  └─────────────────────┘ │
│   两者同进程，经进程内接口调用（无 IPC），但模块解耦    │
└───────────────────────────────────────────────────────┘
```

### 4.2 进程职责（建议命名）

| 进程 | 角色 | 命名建议 | 关键职责 |
|---|---|---|---|
| **gateway** | 接入进程 | `tqsdk-gw` | 持有 CTP API（必须与 CTP 同进程，SPI 回调线程）、天勤 WS。一个进程可托管多个网关实例（md+td+多账户）。只做接入与协议翻译，**不持有行情截面**。 |
| **core + serve** | 数据服务进程 | `tqsdk-node` | 权威数据截面 + 对外接口。core 层做 diff 合并、快照生成、脏位管理、命令路由；serve 层做 HTTP/WS 暴露、订阅管理、查询/下单入口。**唯一有权写数据的地方**。两层同进程、代码边界清晰。 |

> 关键点：core 与 serve 不做跨进程 IPC——serve 直接读 core 的内存截面（`DataManager`），
> 通过进程内接口获取差分/快照；只有 gateway 与 core 之间才是真正的 IPC 边界。

### 4.3 通信拓扑

```text
        ┌───────────────────────────────┐
        │   node (core + serve, 同进程)   │
        │   ┌─────────┐    ┌──────────┐  │
        │   │  core   │◀───│  serve   │  │  serve 读 core：进程内调用
        │   └────┬────┘    └──────────┘  │
        └────────┼───────────────────────┘
                 │ IPC (UDS)：subscribe/command 下行，push 上行
        ┌────────▼───────────────────────┐
        │            gw (接入进程)         │
        │  tq md | ctp md | ctp td         │
        └──────────────────────────────────┘
```

- **node 内部（core <-> serve）**：进程内函数调用，零 IPC。serve 订阅 core 的差分流，把 Snapshot/Diff 转成对外格式。
- **node <-> gw（core <-> gateway）**：全双工 IPC。core 下发 subscribe/command；gw 上推 MarketDataPush/TradePush/GatewayEvent。
- **gw 不直连 serve**：即便同机也只是接入进程，所有数据必经 core 合并落库，保证"单一权威数据源"。

### 4.4 IPC 传输选型

| 方案 | 优点 | 缺点 | 建议 |
|---|---|---|---|
| **UDS (Unix Domain Socket)** | 已有 `UdsChannel` 可复用；可靠有序；跨用户安全性好；零配置 | 单机；拷贝开销 | **首选，跨语言/跨机可换 TCP** |
| 共享内存 + 环形缓冲 | 极低延迟、零拷贝 | 复杂度高、需同步原语、跨机不可用 | 行情高频场景作为**二期优化**（可选快速通道） |
| TCP + MessagePack | 可跨机分布式 | 延迟略高、需 TLS/鉴权 | 分布式部署时启用 |

**结论**：一期 gateway<->node 全部用 **UDS + MessagePack**（复用现有帧协议），把"零拷贝共享内存"作为行情端到端延迟优化项留到性能阶段。

### 4.5 协议映射（复用 `ipc/frame.h` 已有设计）

现有 `FrameType` 覆盖了 gateway<->node 这条唯一的 IPC 边界，几乎不用改：

| 方向 | FrameType | 用途 |
|---|---|---|
| node(core) -> gw | `Subscribe` / `Command` | 订阅转发、命令转发 |
| gw -> node(core) | `MarketDataPush` / `TradePush` / `GatewayEvent` | 原始推送、交易回报、状态 |
| 双向 | `Hello/HelloAck` / `Heartbeat` / `Goodbye` | 握手、保活、优雅关闭 |

> 由于 core 与 serve 同进程，`Snapshot` / `Diff` / `CommandAck` 等**不必走 IPC**，直接作为进程内接口
> 的返回值/差分流消费即可（帧类型保留定义，供未来若再拆 serve 时复用）。
> 仍需补充：**SubscribeAck 携带订阅结果**、**gap 检测/重放**（见 4.6）。

### 4.6 关键设计问题：断线与一致性

两进程架构下，需要处理的断线场景只有 **gw<->node** 一条：

1. **gw<->core 断线**：core 需缓存"当前有效订阅集合"，重连 gw 后重发 Subscribe；gw 侧网关需重连柜台并重放订阅（已有 pending_subs_ 机制，需扩展到命令）。
2. **gap 检测**：每个 push 带单调递增 `seq`（core 维护全局序列号）。gw/node 检测 seq 跳变 -> 请求补发；行情可重建（重新订阅拿快照），交易回报**不可丢**（重连后 QueryOrders/持仓对齐）。
3. **交易命令必达**：`Command` 走"应用层重传 + Ack 确认 + QueryOrders 兜底"（协议表已设计，需实现）。
4. **core/serve 同进程无断线问题**：serve 直接读内存，无需 Snapshot 重建逻辑——这是两进程架构相对三进程的**显著简化**。

---

## 5. 现有抽象需要做的改造

### 5.1 网关接口：从"进程内接口"到"可远程代理的接口"

**现状**：`MarketDataGateway` / `TradingGateway` 是纯虚类，`TqApi` 直接持有 `unique_ptr<...>`。

**改造目标**：让同一套接口既能"进程内直连"，也能"通过 IPC 远程代理"。

- **方案**：为每个网关额外实现一个"远程代理网关"：
  - `RemoteMdGateway : MarketDataGateway`：内部持有一条 `IIpcChannel`，`subscribe()` -> 发 `Subscribe` 帧；后台读协程把 `MarketDataPush` 解码成 `MarketDataEvent` 推入本地 `output()` 通道。
  - `RemoteTdGateway : TradingGateway`：`insert_order()` -> 发 `Command` 帧并等待 `CommandAck`；`TradePush` 解码为 `TradeEvent`。
- **好处**：`TqApi` / `core` 的上层代码**几乎不改**（仍面向接口编程）。进程内直连与远程代理是同一接口的两个实现。
- **注意**：现有接口返回 `awaitable<T>`，远程代理天然适配（发帧 + 等待响应）。但 `output()` 返回 `Channel<...>&` 的引用语义，代理需自行持有内部 Channel，可行。

### 5.2 TqApi 的定位变化

**现状**：`TqApi` = 门面 + 数据截面 + 合并引擎 + 后台 loop，全耦合。

**拆分后**（core 与 serve 同在 node 进程）：
- **core 层** 内部保留 `DiffMergeEngine` + `DataManager`，通过**进程内接口**向 serve 层提供快照/差分流（不走 IPC）。
- `TqApi` 的"用户门面"职责落在 **serve 层**，与 core 层同进程但代码分区。
- 需要把 `TqApi` 拆成两层职责：
  1. **CoreEngine**（core 层）：网关接入（远程网关代理）-> 合并 -> 数据截面 -> 生成对外 Snapshot/Diff 流。
  2. **Serve 层 / TqApi 门面**：同进程内订阅 CoreEngine 的差分流、重建本地只读视图、对外提供 `get_quote/wait_update` 等接口（API 兼容）。

> 这是**最大的一处重构**。建议引入新的中间抽象层，例如 `DataFeed`（本地数据源）与 `CommandSink`（命令出口），让门面既能对接本地 `DataManager` 直接读，也能对接 `CoreEngine` 的差分流——**两条路径都在进程内，无跨进程 IPC**。

### 5.3 数据面：Snapshot/Diff 序列化

- 现在 diff 是 `nlohmann::json`。跨进程建议 **MessagePack**（体积小、无 schema 开销），帧编码已有 `FrameEncoding::MessagePack` 位。
- 需要为 domain 实体补 `to_msgpack` / `from_msgpack`（可复用 `msg.h` 的 aid 驱动思路，或直接对 `Quote/Order/...` 做序列化）。
- **快照格式**：`{实体类别: {key: 实体}}`；增量：与现有 trade diff 同构的 JSON/msgpack。
- **脏位跨进程**：`changed_fields` 是 uint64 位标记，天然可序列化；但要明确"serve 侧重建时如何置脏"。

### 5.4 Channel 与字节流层的复用

- `Channel<T>` 继续作为**进程内**的异步队列（网关解码后、core 分发前）。
- 进程间原语是 `IIpcChannel`（字节帧），在其上做**协议分发层**：`FrameType -> 具体消息` 的路由器。
- 建议新增 `ipc/ipc_session`（一条连接上的握手/心跳/帧路由/seq 管理），把 `UdsChannel` 从"裸通道"提升为"会话"。

### 5.5 线程模型明确化

- **gw 进程**：至少两类线程——CTP 的 SPI 回调线程（SDK 管理）+ Asio io_context 线程池。必须有 TaskQueue 桥接（README 已设计，需落地）。
- **node 进程**：Asio io_context 单/多线程 + strand 保护数据截面写路径；HTTP/WS 的 serve 层在 io_context 上并发，访问 core 数据经 strand/dispatch 串行化。
- 所有跨线程数据交接走 `asio::post(strand, ...)`，禁止裸 mutex 共享可变状态。

---

## 6. 阶段路线图

> 说明：阶段编号延续现有（已完成 0~3 及部分 5）。以下为**后续阶段**，每阶段给出目标、工作项、验收标准。

### 阶段 4：CTP 接入（补齐上层交易能力）
**目标**：真实柜台接入，先交易后行情，Bridge CTP 同步世界到 Asio。

**工作项**：
- [ ] `ctp_task_queue`：线程安全队列，SPI 回调只入队不阻塞。
- [ ] `ctp_spi_impl`：`CThostFtdcTraderSpi` / `MdSpi` 实现，回调转任务。
- [ ] `CtpTdGateway`：登录/看穿式认证/结算确认、`insert_order`/`cancel_order`/`query_*`、回报翻译为 trade diff。
- [ ] `CtpMdGateway`：订阅、tick 翻译为 market diff。
- [ ] 重连与前序状态重放（当前持仓/在途单查询对齐）。
- [ ] 用**模拟柜台**（SimNow 或自建 mock CTP SPI）做离线集成测试。

**验收**：SimNow 环境下能下单、撤单、收到成交回报，`ctp_*` 测试覆盖 TaskQueue 与 SPI 时序。

### 阶段 5：IPC 协议端到端（拆分的前置）
**目标**：把 `ipc/frame.h` 的协议真正跑起来，先做"gw 进程 + node 进程"直连验证。

**工作项**：
- [ ] `ipc/ipc_session`：握手、心跳、seq、FrameType 路由。
- [ ] 定义各消息的 msgpack body schema（Subscribe/MarketDataPush/TradePush/Command/GatewayEvent）。
- [ ] `RemoteMdGateway` / `RemoteTdGateway`（proxy 实现接口）。
- [ ] 本地起 gw + node 两进程，跑通"订阅 -> push -> 合并 -> 下单 -> 回报"。
- [ ] gap 检测 + 重连补发路径。

**验收**：端到端测试（两进程）复现现有单进程 e2e 的全部场景。

### 阶段 6：两进程拆分落地
**目标**：gw / node 两个可执行文件，core 与 serve 在 node 进程内代码分区。

**工作项**：
- [ ] 拆出 `tqsdk-gw` / `tqsdk-node` 两个 main。
- [ ] `CoreEngine`（node.core 层）：网关接入（远程网关代理）+ 合并 + 对外 Snapshot/Diff 流。
- [ ] serve 层（node.serve 层）：进程内消费 CoreEngine 流，保持 `get_quote/wait_update` 语义。
- [ ] core / serve 代码边界：目录（`core/` 与 `serve/`）+ 接口隔离，禁止 serve 直接改 core 内部状态。
- [ ] 进程编排：启动顺序、健康检查、优雅退出（Goodbye 帧）。

**验收**：两进程协同，策略示例改成"经 node 拿数据"，行为与单进程等价；core/serve 模块可独立单测。

### 阶段 7：服务层（HTTP/WebSocket，node 进程内）
**目标**：在 node 进程内对策略进程/图表前端提供服务（不单独拆进程）。

**工作项**：
- [ ] 选型 HTTP 框架（建议 Boost.Beast 自带 HTTP/WS，避免新依赖；或 Drogon/Crow 视需求）。
- [ ] REST：`GET /quote/{symbol}`、`GET /kline/{symbol}/{dur}`、`GET /account`、`POST /order`、`DELETE /order/{id}`。
- [ ] WS：`/stream` 推送 diff + 心跳；前端订阅任意标的。
- [ ] 鉴权（token）、限流、CORS（图表场景）。
- [ ] 订阅管理：serve 层维护"下游订阅"引用计数，聚合后向 core 发起/退订。
- [ ] serve 层直接读 core 内存截面（进程内），不引入额外 IPC。

**验收**：curl / wscat 能拿到行情、下单；一个最小网页图表 demo 能实时刷新。

### 阶段 8：深化测试与容错
**目标**：让拆分后的系统可运维、可回归。

**工作项**：
- [ ] 多线程 io_context + strand 并发测试。
- [ ] 重连/断线/gap/命令重传的系统测试（假服务器注入故障）。
- [ ] soak test（长稳、内存、句柄泄漏）。
- [ ] CTP 模拟柜台回归测试。

**验收**：故障注入下数据不丢（交易）/可重建（行情），长稳 24h 无泄漏。

### 阶段 9：性能优化
**目标**：降低端到端延迟、提升吞吐。工作项见第 8 章。
**验收**：给出延迟/吞吐基线对比表。

---

## 7. 配置项设计

拆分后配置复杂度上升，建议**统一配置模型**（TOML/YAML + 环境变量覆盖），按进程配置 + 共享公共段。

### 7.1 配置文件分层

```text
config/
  common.toml      # 日志、IPC 路径前缀、序列化编码
  gateway.toml     # gw 进程：各网关实例（tq md / ctp md / ctp td + 账户）
  node.toml        # node 进程：core 段（数据截面/快照策略/订阅上限）+ serve 段（HTTP/WS/鉴权）
```

### 7.2 关键配置项

| 段 | 键 | 说明 | 默认 |
|---|---|---|---|
| common.log | level / colored / file / rotate_mb / rotate_keep | 日志（复用现有 LogOptions） | info |
| common.ipc | transport(uds/tcp) / uds_dir / max_payload / heartbeat_sec | IPC 传输 | uds, 2MB, 30s |
| common.codec | encoding(json/msgpack) | 线格式 | msgpack |
| gateway.tq_md | host / port / path / reconnect_ms / peek_interval_ms | 天勤行情 | free-openmd |
| gateway.ctp_td | broker_id / user / password / auth_code / app_id / td_front / md_front | CTP 交易 | — |
| gateway.ctp_md | 同上 md | CTP 行情 | — |
| gateway.multi | accounts[]（多账户） | 多柜台并行 | — |
| core.data | snapshot_on_subscribe(bool) / kline_default_width / max_symbols / seq_check | 截面策略 | true |
| core.router | order_seq_prefix / command_timeout_ms / retry | 命令路由 | rid_ |
| serve.http | bind / port / auth_token / rate_limit_qps / cors_origin | HTTP 服务 | 127.0.0.1:8080 |
| serve.ws | path / heartbeat_sec / max_clients | WebSocket | /stream |

### 7.3 配置实现建议
- 引入 TOML 解析（`toml++`，轻量、header-only）或继续 JSON（已有 nlohmann）。**优先 toml++**（可读性好，适合运维）。
- 支持 `TQSDK_<SECTION>_<KEY>` 环境变量覆盖（便于容器/CI），敏感项（密码）只从环境变量/密钥文件读，**不落配置文件**（呼应之前 `.env` 的教训）。
- 配置校验：启动即校验必填项，失败快速退出并打印清晰错误（fail-fast）。
- 每个进程用同一套 `Config` struct + 子集校验（gw 只读 gateway 段；node 读 core+serve 段）。

---

## 8. 性能优化方向

> 建议在阶段 9 用**基线数据驱动**，按"先测后优"顺序做。下列按预期收益排序。

### 8.1 序列化与拷贝
- diff 全链路目前是 `nlohmann::json`（堆分配重）。跨进程改 **MessagePack**，并考虑 **flatbuffers/Cap'n Proto** 做零解析（行情高频字段）。
- 减少 `std::vector<uint8_t>` 的拷贝：帧 header + payload 用 scatter/gather（`async_write` 多 buffer，UDS 已支持），payload 直接指向序列化 buffer，避免二次拷贝。
- 实体热路径避免 `std::string` 键的重复哈希：考虑 `string_view` + 稳定 interned id（symbol -> 紧凑 uint32）。

### 8.2 数据面
- `EntityStore` 用开放寻址哈希或 `flat_hash_map`（absl/ankerl）替代 `std::unordered_map`，减少指针追逐。
- diff 合并批量应用（一次 push 内多实体变更合并到同一事务，减少 notify 次数）。
- K 线用环形缓冲（固定容量、无裁剪 vector 搬移）。

### 8.3 传输层
- **共享内存环形缓冲**作为 gw<->core 的行情快速通道（零拷贝、无系统调用），交易走 UDS 保可靠；即"快慢双通道"。
- UDS 设置 `SO_SNDBUF/RCVBUF`、批量聚合（把多个小 diff 合并成一个帧，微批处理）。
- 引入 **seq + 批处理**：core 把短时间窗内多个 diff 合成一帧发 serve，降低帧头/唤醒开销。

- io_context 多线程 + 每网关 strand；CPU 密集（序列化）可分流到线程池，避免阻塞 IO 线程。
- 避免 `asio::post` 抖动：高频路径用 `dispatch`（已在执行器线程内时免投递）。
- 定时器合并（心跳、重连退避用单个 steady_timer，而非多个）。

### 8.5 内存与稳定性
- 对象池化高频消息（帧、diff 临时对象），减少分配。
- Channel 有界化 + 背压：行情通道满时按策略丢旧帧（行情可丢）、交易通道满时阻塞/告警（交易必达）。
- 监控指标导出（Prometheus / statsd）：队列深度、延迟分位、seq gap 次数。

### 8.6 性能基线建议先建立

- 单条 diff 端到端延迟（gw 收到 -> node serve 层对外发出）。
- 吞吐（diff/s），用 mock 网关压测，避免受真实服务器限制。
- 深拷贝次数、每帧线性分配次数（用 heaptrack/massif）。

---

## 9. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| 拆分带来的复杂度 | 两进程 + IPC + 一致性，调试成本高 | 保留"单进程直连"模式用于开发/测试（同一接口两实现）；两进程仅在需要网关隔离时启用 |
| 一致性/断线 | gw<->node 断线、gap、命令重传 | 强 seq + 重订阅重建 + 交易重传；阶段 5/8 专项测试 |
| CTP 同进程约束 | SPI 回调线程序、不可阻塞 | 仅 gw 进程挂 CTP；TaskQueue 桥接；Asio 线程池隔离 |
| 性能回退 | IPC 拷贝增加延迟 | 快慢双通道（共享内存行情）、msgpack、微批处理 |
| 配置漂移 | 多进程配置不一致 | 统一 schema + 共享 common 段 + 启动校验 |
| core/serve 边界腐化 | 同进程内两层耦合，退化为大泥球 | 目录（core/ serve/）+ 接口隔离 + core/serve 各自单测；禁止 serve 改 core 内部状态 |
| 测试缺口堆积 | IPC/CTP/并发未测就拆分 | 拆分前必须补 IPC 协议 e2e；并发/故障注入与拆分并行推进 |

### 权衡建议（给决策）
- **进程拆分粒度**：已定**两进程**（gw 独立、core+serve 合一）。理由：serve 的 HTTP 层与 core 数据层耦合紧，拆成独立进程只增加 IPC 与运维成本、收益有限；在 node 进程内用目录+接口划清 core/serve 边界即可。若未来确有"多前端/远程访问"需求，帧协议已预留 `Snapshot/Diff/CommandAck`，可再平滑拆出 serve。
- **是否引入共享内存？** 一期不做，UDS + msgpack 足够；行情延迟成为瓶颈时再加。
- **HTTP 框架**：优先 Boost.Beast（零新依赖、与现有 Asio 融合），除非需要复杂路由/中间件才引 Drogon。

---

## 10. 附录：文件清单与关键决策

### 10.1 当前源码地图

```text
include/tqsdk/
  tqapi.h                 门面（将拆分 core 层 CoreEngine / serve 层门面）
  msg.h                   天勤消息（aid 驱动）
  types.h / update_signal.h
  channel/channel.h       进程内异步通道
  data/                   EntityStore / DataManager / DiffMergeEngine / field_table
  domain/                 Quote/Order/Trade/Position/Account/Kline/Sentinel/枚举
  gateway/
    market_data_gateway.h / trading_gateway.h   抽象接口（拆分保持）
    tq/tq_md_gateway.h                          天勤行情
    sim/sim_td_gateway.h                        模拟撮合
    mock/*                                      mock 测试网关
  ipc/
    frame.h               帧类型表（已经为拆分设计好）
    ipc_channle.h         IIpcChannel 抽象
    uds_channel.h         UDS 实现
  util/                   log / serde / async_mutex / spawn_logged
src/                      对应实现
tests/                    ~128 用例
examples/demo.cpp
scripts/probe_*.py        协议探测
third_party/ctp/          CTP SDK（有头与 so，网关未实现）
```

### 10.2 已定死的关键决策（沿用）
- 统一 Asio 协程模型（awaitable），Channel 不引入额外线程。
- diff 合并唯一入口 `DiffMergeEngine::apply`，脏位 `changed_fields` 追踪。
- 网关只推 diff 不落库；合并集中在 core。
- `wait_update` 语义 = "自上次 wait 以来"，进入下一轮清位。
- `insert_order` 本地占位立即可见，ack 失败即错误终态。
- 模拟器与真实网关同构（都通过 diff 推状态）。
- IPC 帧 `[len u32 BE][type u8][encoding u8][payload]`，len=2+payload。

### 10.3 新增/需创建的模块（拆分相关）

```text
include/tqsdk/ipc/
  ipc_session.h           会话（握手/心跳/seq/路由）          [待建]
  protocol.h              各 FrameType 的 msgpack body         [待建]
  remote_md_gateway.h     远程行情代理（实现 MarketDataGateway） [待建]
  remote_td_gateway.h     远程交易代理（实现 TradingGateway）    [待建]

include/tqsdk/core/       node 进程内的 core 数据层
  core_engine.h           核心（远程网关->合并->对外差分流）    [待建]
  order_router.h          命令路由（serve->core->gw）           [待建]

include/tqsdk/serve/      node 进程内的 serve 服务层
  api_server.h            HTTP/WS 服务（基于 core 差分流）       [待建]
  subscription_mgr.h      下游订阅引用计数 -> core 订阅          [待建]

include/tqsdk/ctp/
  ctp_task_queue.h        SPI 回调 -> Asio 桥接                [待建]
  ctp_spi_impl.h          CThostFtdc Trader/Md Spi 实现        [待建]
  ctp_td_gateway.h        CTP 交易网关                         [待建]
  ctp_md_gateway.h        CTP 行情网关                         [待建]

apps/
  gw_main.cpp             tqsdk-gw 入口                        [待建]
  node_main.cpp           tqsdk-node 入口（内含 core + serve）  [待建]
```

### 10.4 一句话优先级
1. **先补 IPC 协议端到端 + 两进程验证**（阶段 5）——拆分的硬前置。
2. **并行推进 CTP 接入**（阶段 4）——补齐上层真实交易能力。
3. **再落两进程（gw + node）+ node 内服务层**（阶段 6/7）。
4. **最后做并发/容错/性能深化**（阶段 8/9）。
