# Tqsdk-Cpp

针对 TqSdk 的核心设计思想——基于异步队列的数据流管道 + 可替换组件——在 C++ 中需要一套比 Python 更显式的抽象机制来实现同等甚至更优的可替换性与异步性能。以下设计以 Boost.Asio 作为统一异步执行引擎，围绕“通道 (Channel)”和“网关插件 (Gateway Plugin)”两个核心抽象展开。

## 架构设计

### 整体分层

```text
┌─────────────────────────────────────────────────────────────┐
│                     用户策略层 (Strategy)                     │
│   订阅行情 / 查询账户 / 报单撤单 / 等待事件 (co_await)          │
└──────────────────────────┬──────────────────────────────────┘
                           │ TqApi 门面 (Facade)
┌──────────────────────────▼──────────────────────────────────┐
│                     TqApi 核心层                              │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ DataManager │  │ OrderRouter │  │ DiffMergeEngine     │  │
│  │ (内存数据截面)│  │ (报单路由)   │  │ (Diff 合并引擎)      │  │
│  └──────┬──────┘  └──────┬──────┘  └─────────────────────┘  │
│         │                │                                   │
│    ┌────▼────────────────▼────┐                              │
│    │    Channel<T> (异步通道)  │  ← 核心数据流抽象             │
│    └────────────┬─────────────┘                              │
└─────────────────┼────────────────────────────────────────────┘
                  │ 统一推送/拉取接口
┌─────────────────▼────────────────────────────────────────────┐
│                   Gateway 抽象层                              │
│  ┌─────────────────────┐  ┌─────────────────────────────┐   │
│  │  MarketDataGateway  │  │  TradingGateway             │   │
│  │  (行情网关接口)       │  │  (交易网关接口)              │   │
│  └──────────┬──────────┘  └──────────────┬──────────────┘   │
│             │                            │                   │
│   ┌─────────┴─────────┐        ┌─────────┴─────────┐        │
│   │ TqMdGateway       │        │ TqTdGateway       │        │
│   │ (天勤 WebSocket)   │        │ (天勤 WebSocket)   │        │
│   ├───────────────────┤        ├───────────────────┤        │
│   │ CtpMdGateway      │        │ CtpTdGateway      │        │
│   │ (CTP MdApi 包装)   │        │ (CTP TdApi 包装)   │        │
│   ├───────────────────┤        ├───────────────────┤        │
│   │ SimMdGateway      │        │ SimTdGateway      │        │
│   │ (本地模拟/回测)     │        │ (本地撮合)         │        │
│   └───────────────────┘        └───────────────────┘        │
└──────────────────────────────────────────────────────────────┘
                  │
┌─────────────────▼────────────────────────────────────────────┐
│              Asio io_context (统一异步执行引擎)                │
│         strand / timer / socket / coroutine 调度             │
└──────────────────────────────────────────────────────────────┘
```

### 核心抽象一：Channel<T> —— 数据流管道

TqSdk 用 TqChan（本质是 asyncio.Queue）连接组件。在 C++ 中，将 Channel<T> 设计为 基于 Asio 的异步有界/无界队列，同时支持协程风格的 async_receive 和回调风格的 async_receive(callback)：

```cpp
template <typename T>
class Channel {
public:
    explicit Channel(asio::any_io_executor ex, std::size_t capacity = 0);

    // 生产者：线程安全/异步安全的异步发送
    asio::awaitable<void> async_send(T item);

    // 消费者：协程风格
    asio::awaitable<T> async_receive();

    // 消费者：回调风格（兼容非协程代码）
    template <typename Handler>
    void async_receive(Handler&& handler);

    void close();                     // 关闭通道，唤醒所有等待者
    bool is_closed() const noexcept;
};
```

关键设计点：Channel 内部使用 asio::steady_timer 或 Asio 的信号机制实现异步等待，不引入额外的线程或条件变量。整个系统的并发由 io_context 的线程池驱动，避免 TqSdk Python 版中 asyncio 事件循环单线程调度的瓶颈。

### 核心抽象二：网关插件接口

这是实现“CTP 接入、模块可替换”的关键。将行情和交易拆分为两个独立接口，行情与交易可以来自不同后端（例如天勤行情 + CTP 交易，或 CTP 行情 + 模拟交易）：

```cpp
// 行情网关统一接口
class MarketDataGateway {
public:
    virtual ~MarketDataGateway() = default;

    virtual asio::awaitable<void> start() = 0;
    virtual asio::awaitable<void> stop() = 0;

    virtual asio::awaitable<void> subscribe(const std::vector<std::string>& symbols) = 0;

    // 网关内部将收到的行情 diff 推入此通道
    virtual Channel<MarketDataDiff>& output() = 0;
};

// 交易网关统一接口
class TradingGateway {
public:
    virtual ~TradingGateway() = default;

    virtual asio::awaitable<void> start() = 0;
    virtual asio::awaitable<void> stop() = 0;

    // 请求-响应式操作（报单、撤单、查询）
    virtual asio::awaitable<OrderResult> insert_order(const OrderRequest& req) = 0;
    virtual asio::awaitable<OrderResult> cancel_order(const CancelRequest& req) = 0;
    virtual asio::awaitable<AccountInfo> query_account() = 0;
    virtual asio::awaitable<std::vector<Position>> query_positions() = 0;

    // 交易事件（成交回报、订单状态变化、错误）推送通道
    virtual Channel<TradeEvent>& event_output() = 0;
};
```

TqApi 构造时注入具体的网关实现，上层策略代码完全不需要感知底层是 TqSdk 还是 CTP。这与 TqSdk 用 TqSim 替换 TqAccount 的思路完全一致，但在 C++ 中通过虚接口 + 依赖注入实现，编译期和运行期都有清晰的边界。

### 核心抽象三：TqApi 门面与 Diff 合并引擎

`TqApi` 是对用户暴露的唯一入口，内部持有：

- `std::unique_ptr<MarketDataGateway> md_gateway_`
- `std::unique_ptr<TradingGateway> td_gateway_`
- `DataManager data_manager_`（对应 TqSdk 的 TqApi._data，单一数据截面）
- `DiffMergeEngine diff_engine_`（递归合并 diff 到内存数据）

TqApi 在构造时启动一个或多个 后台 Asio 协程，持续从 md_gateway_->output() 和 td_gateway_->event_output() 消费数据，经过 Diff 合并后写入 DataManager，再通过监听器通道通知用户代码。用户通过 TqApi 提供的 get_quote() / insert_order() 等接口操作，这些接口返回 asio::awaitable<T>，天然支持协程风格：

```cpp
asio::awaitable<void> strategy(TqApi& api) {
    auto quote_ch = api.get_quote_channel("SHFE.rb2610");
    while (true) {
        auto diff = co_await quote_ch.async_receive();
        // 处理行情更新
    }
}
```

## CTP 接入的关键：CtpGateway 的异步适配

CTP API 是同步回调驱动的 C++ 库，且回调线程由 CTP SDK 管理，回调中不能阻塞（阻塞可能导致缓冲区溢出甚至崩溃）。因此 CtpTdGateway 和 CtpMdGateway 需要一个 Task Queue + Asio post 的桥接层：

```text
CTP SPI 回调线程
       │
       ▼
[TaskQueue] (线程安全队列，CTP 回调只做入队，绝不阻塞)
       │
       ▼
asio::post(io_context, ...)  或  dedicated dispatch 协程
       │
       ▼
Gateway 内部处理 → 写入 Channel<TradeEvent> / Channel<MarketDataDiff>
```

这个模式与 vnpy_ctp 的 Task Queue 架构一致：CTP 回调只负责将数据封装为任务入队，由独立的消费者将数据投递到 Asio 执行上下文中进行后续处理。区别在于，vnpy 的任务队列最终服务于 Python GIL 边界，而这里的任务队列服务于将回调线程的同步世界安全地桥接到 Asio 的异步世界。

### 异步执行模型建议

- 单一 io_context + 多线程 run() 作为全局执行引擎。所有网关的 Asio 操作、通道收发、定时器都绑定到同一个 io_context。
- 每个网关使用 asio::strand 保证同一网关内部的事件处理串行化，避免对网关内部状态（如 CTP API 实例）的并发访问。
- C++20 协程：如果编译器支持（GCC 11+ / Clang 14+ / MSVC 2022），所有网关接口和 TqApi 的公共 API 优先使用 asio::awaitable<T>。对于不支持协程的调用方，保留 callback 风格的重载。
- Channel 的线程安全性：如果 CTP 回调线程需要直接向 Channel 投放数据（绕过 Asio post），Channel 的 async_send 必须通过 asio::post 转发到绑定的 executor 上执行，确保通道内部状态不被跨线程并发修改。

## 使用与运行

### 构建

```bash
# 测试（默认）
cmake --preset default
cmake --build build -j

# 同时构建示例程序
cmake -S . -B build -DTQSDK_BUILD_EXAMPLES=ON
cmake --build build -j --target tqsdk_demo
```

### 日志

库内统一使用 `spdlog`。初始化一次即可让库与业务代码共享同一套配置：

```cpp
#include "tqsdk/util/log.h"

tqsdk::LogOptions opts;
opts.level   = "info";          // 留空则读环境变量 TQSDK_LOG_LEVEL，再缺省 info
opts.colored = true;            // 控制台着色
opts.file    = "tq.log";        // 可选：额外写滚动文件（>1MB 轮转，留 3 份）
tqsdk::setup_logging(opts);
```

- 级别优先级：`opts.level` > 环境变量 `TQSDK_LOG_LEVEL` > `info`。
- 测试可执行文件在 `main()` 启动即调用 `setup_logging()`；运行时可
  `TQSDK_LOG_LEVEL=debug ./build/tqsdk_tests` 观察详细日志，或 `off` 静默。
- 日志格式默认 `时间 [级别] [线程] 来源 正文`；`warn` 及以上立即刷盘。

### 实时行情示例（quote + kline）

`examples/demo.cpp` 连接天勤免费行情服务器，实时打印盘口与 K 线：

```bash
./build/tqsdk_demo                          # 默认 SHFE.rb2610，60s K 线
./build/tqsdk_demo SHFE.cu2611 180000000000 # 自定义合约 + 周期(纳秒)
```

核心 API 语义（对齐 TqSdk）：

```cpp
api.get_quote("SHFE.rb2610");                                   // 订阅盘口，返回引用
KlineStream* k = api.get_kline_serial("SHFE.rb2610", 60'000'000'000LL, 20);
                                                                // 取序列（按需创建+订阅）
while (true) {
    co_await api.wait_update();                                 // 等待任意 diff 合并
    auto& q = api.get_quote("SHFE.rb2610");                     // 读盘口
    if (k && !k->bars.empty()) { /* 读 k->bars.back() */ }      // 读 K 线
}
```

- `get_kline_serial(symbol, duration_ns, view_width)`：创建本地 `KlineStream`
  （`capacity = view_width`，超出自动裁剪最旧 bar）并发起订阅。
- `get_kline(symbol, duration_ns)`：纯读，未创建时返回 `nullptr`。
- 序列的 `changed_fields & KlineChangeDetail::Appended` 可判断是否新增了 bar。

### 天勤 K 线协议要点（经真实连接探测确认）

- 订阅走 `set_chart`：`{aid, chart_id, ins_list, duration(ns), view_width}`；
  `ins_list=""` 表示取消。**`chart_id` 必须是 32 位小写十六进制**
  （对齐官方 `_generate_uuid()`），含 `@`/`.` 等特殊字符会被服务器断开。
- 返回 `rtn_data.klines` 形态为 `{sym:{duration_ns:{"data":{"<bar_id>":bar}}}}`，
  网关层将其翻译成引擎数组形态 `{sym:{duration_ns:[bar,...]}}`（按 bar_id 数值升序）。
- 首帧给全量历史，后续增量给单根，元信息帧无 `data`（已忽略）。
- 流控：每发一次 `peek_message` 才回一帧，读完需再 peek。

### 联网测试

真实行情测试默认跳过，设环境变量启用：

```bash
TQSDK_RUN_NETWORK=1 ./build/tqsdk_tests --gtest_filter='TqMdNetwork.*'
```

- `TqMdNetwork.SubscribeQuoteReceivesData`：订阅盘口，校验 finite `last_price`。
- `TqMdNetwork.KlineSerialReceivesBars`：`get_kline_serial` 订阅，校验收到 bars。

建议在交易日运行，非交易时段服务器仅回历史 K 线、盘口可能不刷新。

## 两进程模式（tqsdk-gw + tqsdk-node）与 IPC / 服务层

除"进程内直连"外，项目支持把**接入**与**数据服务**拆成两个进程，用 IPC 连接：

```text
tqsdk-gw（接入进程）                         tqsdk-node（数据服务进程）
  真实网关(TqMd/SimTd)                         IpcClient -> Remote{Md,Td}Gateway
  GatewayIpcServer（桥接上/下行）              TqApi -> DiffMergeEngine -> DataManager
  IpcSession(Gateway 角色)  <=== UDS/TCP ===>  IpcSession(Node 角色)
                                               HttpServer（REST serve 层）
```

- **单一权威数据源**：只有 node 合并并持有内存截面；gw 只做接入与协议翻译。
- **同一接口两种实现**：`RemoteMdGateway`/`RemoteTdGateway` 与 `TqMdGateway`/`SimTdGateway`
  实现同一套网关接口，`TqApi` 无感知。
- **多 node 共享一个 gw 网关**：行情网关用**引用计数退订 + 投递前过滤 + peek 闸门**，保证 node 断联后
  不再产生无人消费的行情入队（详见 `docs/MODULES.md` §3.5）。

### 运行

```bash
# 终端 1：接入进程（监听 /tmp/tqsdk-gw.sock，内置 SimTd + 天勤行情，按 config/gw.toml）
./build/tqsdk-gw -c config/gw.toml
# 终端 2：数据服务进程（连 gw，合并截面，暴露 HTTP :8080）
./build/tqsdk-node -c config/node.toml
# 拉数据 / 下单
curl -s localhost:8080/quote/SHFE.rb2610 | jq
curl -s localhost:8080/kline/SHFE.rb2610/60000000000 | jq
curl -s -XPOST localhost:8080/order -d '{"instrument_id":"SHFE.rb2610","direction":0,"offset":0,"volume":1,"price_type":1}'
```

- 两个进程均响应 SIGINT/SIGTERM 优雅退出（关闭网关/连接、排空 io、释放订阅）。
- `scripts/smoke_two_process.sh` 提供一键冒烟。

### WebSocket 实时推送（`/stream`）

node 的 serve 层在同一 HTTP 端口上提供 WebSocket 升级入口，把 core 的
`changed_fields` 增量按客户端订阅过滤后推送。适合图表前端/策略进程订阅任意标的。

```bash
# 终端 3：订阅 rb2610 行情 + 60s K 线 + 交易账户
websocat -t ws://127.0.0.1:8080/stream
> {"op":"subscribe","symbols":["SHFE.rb2610"],
   "klines":[{"symbol":"SHFE.rb2610","duration_ns":60000000000,"view_width":200}],
   "topics":["account","orders","trades","positions"]}
```

消息（文本帧，UTF-8 JSON）。C++ 侧消息结构定义在 `include/tqsdk/serve/ws_protocol.h`：
`WsClientMessage` / `WsServerMessage` 是强类型 variant，配套 `to_json/from_json`、
`parse_ws_client_message()`、`parse_ws_server_message()` 与 `serialize_ws_message()`。

| 方向 | 消息 |
|---|---|
| 服务端 | `{"type":"welcome","protocol":1,"heartbeat_interval_ms":30000}` |
| 客户端 | `{"op":"subscribe","symbols":[...],"klines":[...],"topics":[...]}` |
| 客户端 | `{"op":"unsubscribe","symbols":[...],"klines":[...]}` |
| 客户端 | `{"op":"ping"}` -> 服务端 `{"type":"pong"}` |
| 服务端 | `{"type":"subscribed",...}` / `{"type":"unsubscribed",...}` |
| 服务端 | `{"type":"snapshot","data":{...}}`（订阅成功后的首帧全量） |
| 服务端 | `{"type":"diff","seq":N,"timestamp_ns":...,"data":{...}}`（增量） |

`diff.data` 形态：

- `quotes`: 只含 `changed_fields` 置位的字段；首次快照为完整实体，未知 double 为 `null`。
- `klines`: `{"SYM":{"DUR":{"reset":bool,"bars":[...]}}}`；`reset=true` 时 bars 为全量，
  否则只含最后一根（客户端按 `datetime` 判断替换还是追加）。
- `account` / `orders` / `trades` / `positions`: 仅在变化时出现，内容为完整实体。

实现约定：

- **单一 wait_update 消费者**：`WebSocketHub` 的后台 `push_loop` 是 serve 层唯一调用
  `TqApi::wait_update()` 的协程，避免多客户端抢事件；HTTP 路由不等待更新。
- **引用计数订阅**：多客户端订阅同一 symbol/kline 只向上游订阅一次，
  最后一个客户端退订/断开时才 `release`（与网关引用计数语义对称）。
- **每连接单写者 + 有界队列**：writer 协程独占 socket 写（消息与心跳 ping），
  慢消费者队列满时丢最旧消息；服务端按 `heartbeat_interval_ms` 发 WebSocket ping。
- **executor 归属**：Hub 全部状态与 reader/writer 协程运行在 core strand；
  多 worker 下 HTTP 连接并行，但 WebSocket 数据面仍由 core 串行（详见
  [`docs/THREADING.md`](./THREADING.md)）。
- 配置见 `config/node.toml` 的 `[serve.ws]` 与 `[runtime]`。
- 单测：`tests/serve/ws_stream_test.cpp`（真实 TCP + WebSocket 往返）。

### IPC 帧协议（跨语言契约，勿改）

```text
[4B 大端 length][1B type][1B encoding][payload]
length = 2 + payload.size()          # 含 type+encoding
```

- 消息体先序列化为 `nlohmann::json` DOM，再由 `codec`（JSON / MessagePack）转字节，
  帧头 `encoding` 字节选择编解码器 —— 消息体不耦合具体线格式。
- 帧类型：Hello/HelloAck/Heartbeat/HeartbeatAck/Goodbye/Subscribe/SubscribeAck/
  MarketDataPush/TradePush/Command/CommandAck/GatewayEvent/Resume/ResumeAck/TradeSnapshot。
- `SubscribeBody.release=false` 为订阅（默认）；`true` 表示退订 symbols/klines
  （WebSocket 下游引用计数归零时用，网关按连接内计数释放真实订阅）。
- `MarketDataPush` / `TradePush` 带 `meta{epoch,seq}`，md/td 各自独立序号；
  旧 peer 缺省 `seq=0` 时按 `Ignored` 兼容。

### 序号、断线重连与恢复

- **缺口检测**：`IpcClient` 对 md/td 分别维护 `StreamTracker`，识别
  `InOrder/Duplicate/Gap/EpochChanged`；重复帧丢弃，缺口回调上层。
- **md 恢复**：`RemoteMdGateway` 记录订阅集合，gap/重连时重新下发订阅，
  用上游全量快照重建，不尝试补发旧行情。
- **td 恢复**：优先从 gw 的 `OutboundStream` ring buffer replay；buffer 已淘汰
  或 epoch 变化时，gw 发送 `TradeSnapshot`，node 先 `clear_trading()` 再整体合并。
- **自动重连**：`IpcClient(SessionFactory, ...)` 指数退避重连，成功后重放订阅
  并发送 `Resume`；旧 session 保留到后台协程收尾，避免 UAF。
- 详细设计、状态机、兼容性与观测点见 [`docs/IPC_RECOVERY.md`](./IPC_RECOVERY.md)。

### 多线程运行时

- `[runtime].threads` / `--threads N` 控制 io_context worker 数（默认 1，上限 64）；
- node 使用「每连接 strand 做 HTTP I/O + 单 core strand 做数据面」：
  handler 前 `dispatch(core)`，写回前切回连接 strand；
- TqApi/DataManager/网关/IPC/WebSocketHub 全部绑定 core strand，跨组件无锁；
- gw 当前把网关与会话收敛到一条 core strand，优先保证多连接状态安全；
- 执行器边界、关闭顺序、TSan 命令与后续演进见 [`docs/THREADING.md`](./THREADING.md)。

## 模块文档

逐模块的**目标 / 边界 / 关键约定 / 生命周期**见 [`docs/MODULES.md`](./MODULES.md)；
两进程拆分、阶段路线、性能与风险见 [`docs/ROADMAP.md`](./ROADMAP.md)；
可靠性恢复见 [`docs/IPC_RECOVERY.md`](./IPC_RECOVERY.md)；
多线程模型见 [`docs/THREADING.md`](./THREADING.md)。

### 源码地图

```text
include/tqsdk/
  tqapi.h                   门面：启动网关 / 驱动 loop / 合并 / 查询下单
  types.h msg.h             公共类型 / 天勤 aid 驱动消息
  channel/channel.h         进程内异步通道（有界/无界/close）
  data/                      DiffMergeEngine / DataManager / EntityStore / field_table
  domain/                    Quote/Order/Trade/Position/Account/Kline/Sentinel + 枚举
  gateway/
    market_data_gateway.h    行情网关抽象（subscribe/subscribe_kline/release/output）
    trading_gateway.h        交易网关抽象（insert_order/cancel_order/event_output）
    tq/tq_md_gateway.h       天勤行情（WebSocket/TLS，订阅引用计数 + peek 闸门）
    sim/sim_td_gateway.h     本地撮合模拟器
    mock/*                   脚本化测试网关
  ipc/
    frame.h                 帧格式 + FrameType（跨语言契约）
    ipc_channle.h           IIpcChannel 抽象 / ReceivedFrame
    stream_channel.h        CRTP 帧编解码基类（UDS/TCP 共用）
    uds_channel.h tcp_channel.h
    codec.h                 ICodec：JSON/MessagePack 抽象
    protocol.h              各 FrameType 消息体 schema + encode/decode 辅助
    stream_seq.h            epoch/seq、缺口检测、OutboundStream replay buffer
    ipc_session.h           会话：握手 / 心跳 / 看门狗 / 帧路由
    ipc_client.h            帧分发 + 命令 rid 关联 + 缺口/重连/Resume
    gateway_server.h        gw 侧桥接（真实网关 <-> IPC）
    remote_md_gateway.h     远端行情代理（node 侧）
    remote_td_gateway.h     远端交易代理（node 侧）
  serve/
    router.h                类型化路由（axum 风格）
    http_server.h           REST 服务（node serve 层）
    ws_protocol.h           /stream 强类型消息（WsClientMessage/WsServerMessage）
    ws_stream.h             WebSocketHub：/stream 增量推送 + 订阅引用计数
    json_convert.h          DataManager 实体 -> JSON（REST/WS 共用）
  config/tqapi_factory.h    配置解析 + make_tqapi + 两进程配置
  util/                     spawn_logged / async_mutex / serde / log / runtime
src/                        对应实现
apps/                       gw_main.cpp / node_main.cpp
config/                     gw.toml / node.toml
tests/                      ~222 用例（channel/data/gateway/ipc/serve/e2e/config）
examples/demo.cpp           实时行情示例
scripts/                    两进程冒烟 / 协议探测
```