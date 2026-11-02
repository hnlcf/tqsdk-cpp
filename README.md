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
./build/tqsdk_demo SHFE.cu2611 600000000000 # 自定义合约 + 周期(纳秒)
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