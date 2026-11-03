# 架构与代码质量审查（2026-11）

> 范围：`include/`、`src/`、`apps/`、`tests/`，约 2.5 万行 C++。
> 本轮目标不是重写协议/交易链路，而是先收敛重复、线程边界和输入健壮性，
> 再补可回归的 corner case 测试。

## 1. 总体结构判断

当前分层是合理的：

```text
gw (真实网关/CTP/Tq/Sim)
  │ IPC: frame + seq + replay
  ▼
node (RemoteMd/Td -> TqApi -> DataManager -> HTTP/WS)
  │
  ├─ InstrumentCatalog（合约目录）
  ├─ KlineStore（SQLite K 线缓存）
  └─ serve/router/http/ws
```

保留的设计：

- **单核心数据面 + core strand**：DataManager 不做锁，避免热路径锁竞争；
- **gw/node 两进程**：真实网关可复用，node 专注合并和对外服务；
- **强类型 IPC 帧**：跨语言契约明确；
- **WebSocket 单消费者 `wait_update`**：避免多客户端抢同一轮变化；
- **K 线缓存异步 writer**：SQLite 不进入行情合并线程。

本轮没有为了“看起来更整洁”去拆 CTP 交易状态机；那部分风险高，应单独做
行为回归后再拆。

## 2. 已处理的问题

### 2.1 JSON number/string 解析重复且不严格

原来 `instrument_catalog.cpp` 自带 `json_number`/`json_int`：

- 字符串数字用 `std::stod/std::stoi`，会接受 `12x` 这种前缀；
- `"nan"`/`"inf"` 可能进入价格字段；
- `json_int` 对超出 int 的值直接 `get<int>()`；
- `expired` 等布尔字段用 `j.value`，遇到字符串会抛异常；
- 多处类似的“number/string 判断”散落在 HTTP query 等入口。

已新增：

```text
include/tqsdk/util/json_value.h
```

统一提供：

```text
number / integer / unsigned_integer / boolean / string
number_or / integer_or / unsigned_integer_or / boolean_or / string_or
parse_unsigned / parse_boolean
```

特性：

- 严格拒绝尾随垃圾、NaN、Infinity、溢出；
- 支持协议里常见的数字字符串；
- 布尔支持 bool / 0/1 / true/false / yes/no；
- `uint64` 边界不使用 `double` 近似比较；
- 查询参数和 JSON DOM 共用同一套规则。

已接入：

- `InstrumentCatalog` 字段解析；
- `HttpServer::query_size/query_bool`；
- WebSocket 顶层 `op`/`type` 判别字段。

`DiffMergeEngine` 的 quote/order/trade 数字合并仍保留宏实现：
这是每帧热路径，且已经有单独的字段位图和测试，改成通用 helper 会引入不必要
的抽象和分支成本，本轮不动，后续若要统一应做基准测试再决定。

### 2.2 头文件全局 `using namespace boost`

清理了：

```text
include/tqsdk/channel/channel.h
include/tqsdk/update_signal.h
include/tqsdk/util/async_mutex.h
include/tqsdk/ipc/ipc_channle.h
include/tqsdk/ipc/frame.h
```

这些头文件原来在全局命名空间 `using namespace boost;`，任何包含它们的 TU
都会被动引入 Boost 名称。现在改成命名空间内 `namespace asio = boost::asio;`
或完全移除。

### 2.3 生产 PCH 依赖 gtest / SQLiteCpp

原来 `include/tqsdk/pch.h` 同时包含：

```cpp
#include <SQLiteCpp/SQLiteCpp.h>
#include <gtest/gtest.h>
```

导致 `tqsdk_core` 生产库的预编译头和测试框架耦合，安装/复用边界也不清晰。

已改为：

- `tqsdk/pch.h` 不再包含 gtest / SQLiteCpp；
- `tests/test_pch.h` 只给 `tqsdk_tests` 使用，包含 gtest；
- `SQLiteCpp` 改为 `tqsdk_core PRIVATE`，不再通过 PUBLIC 依赖泄漏给消费者；
- `kline_store.cpp` 自己包含 SQLiteCpp。

### 2.4 WebSocket Hub 生命周期和并发

发现/修正：

- `clients_` 原来只依赖“都在 core strand”约定，但 `stop()`、`full()`、
  `client_count()` 可能从其他线程调用；
- `started_`、`HttpServer::stopped_` 使用普通 bool；
- `stop()` 遍历 clients 时可能与 accept/remove_client 并发；
- `accept()` 在 stop 之后仍可能把连接插入 clients_；
- `shutdown/close_local` 是 `noexcept`，但 `post`/`cancel`/`close` 可能抛异常。

处理：

- `clients_mu_` 保护 clients 表；
- `full/client_count` 加锁；
- `stop()` 先快照 shared_ptr，再逐个 shutdown；
- accept 在锁内二次检查 stopped；
- `started_/stopped_` 改 atomic；
- socket/timer 的关闭路径 catch 异常，避免 noexcept 终止进程。

### 2.5 Channel / UpdateSignal 的 cancel 异常

`Channel::wake_one/close` 和 `UpdateSignal::notify` 是 noexcept，但
`steady_timer::cancel()` 可能通过 Boost.System 抛异常，原实现可能在关闭路径
直接 `std::terminate`。

现在这些路径包住 cancel；`TqApi`、`KlineStore`、`InstrumentCatalog`
析构函数也增加了异常兜底。

### 2.6 Catalog 损坏文件导致流程异常

修正：

- 配置的目录文件存在但损坏时，不再自动回退到 `symbol.json/latest.json`；
  旧快照保持可用，等待下一次刷新；
- 字段使用严格 coercion，单个字段类型错误不会使整个目录加载失败；
- 数组/非对象根节点返回 false，不 swap；
- 增加坏 JSON、混合类型、坏条目、并发读取/重载测试。

### 2.7 测试补充

新增/扩展：

```text
JsonValue.*                         6 个严格解析测试
InstrumentCatalog.*                 坏 JSON、混合类型、并发重载
KlineStore.*                        损坏 DB、并发生产者、freshness、坏 bar
KlineCache.*                        缓存合并、全量回退、stale 缓存
WsProtocol.NumericDiscriminator...  数字 op/type 必须是协议错误
CatalogHttp.QueryNumbers...         HTTP query 严格解析 + 上界 + 坏参数
```

当前结果：

```text
CTP:   265/265
ASan:  265/265
TSan:  252/252
```

## 3. 仍建议后续重构的地方

### 3.1 大文件拆分（优先级中）

当前 hot files：

```text
src/gateway/ctp/ctp_td_gateway.cpp   1164
src/data/diff_merge_engine.cpp         789
src/ipc/gateway_server.cpp             655
src/serve/ws_stream.cpp                651
include/tqsdk/ipc/protocol.h           583
```

建议拆法：

- `ctp_td_gateway.cpp`：
  - `ctp_order_lifecycle.*`（报单/撤单/OrderRef）
  - `ctp_account_query.*`（账户/持仓/成交查询）
  - `ctp_event_emit.*`（CTP -> diff JSON）
- `diff_merge_engine.cpp`：
  - quote / trade / kline 三个 merge 文件；
  - 字段映射表与状态位图放在 `field_table`；
  - 先以“纯移动代码 + 全测试通过”为第一阶段，不改语义。
- `gateway_server.cpp`：
  - 拆出 subscribe/replay/session 三块。
- `ws_stream.cpp`：
  - Client 与 Hub 分文件；
  - subscription ref-count 单独类。

不建议一次性大拆；每个文件单独提交并跑 ASan/TSan。

### 3.2 CTP 字符串/日期解析

`ctp_convert.h` 的 `std::stoi(date.substr(...))`、`ctp_td_gateway.cpp` 的
OrderRef 数字解析仍有重复风险。建议后续统一到 `parse_integer` 风格 helper，
并补：

- 空字符串；
- 短日期/非法日期；
- 超长 OrderRef；
- 非十进制字符。

### 3.3 IPC/WS 模糊测试

当前协议测试是手写样例，建议增加：

- 随机字节 -> `parse_ws_*` / `msg` / IPC frame 必须返回错误而不是崩溃；
- 超大数组/超长字符串/嵌套对象；
- 半帧、错误长度、seq 回绕、重复 Resume。

可直接引入 libFuzzer corpus，先做非持续 CI 的 `--runs=10000`。

### 3.4 线程亲和性断言

目前靠文档约定 core strand。建议在 debug 构建增加：

- `TqApi::data()` 访问断言；
- WebSocketHub 内部 `assert_on_core()`；
- KlineStore reader/writer 边界注释和统计。

不引入运行时锁到热路径。

### 3.5 构建依赖边界

`SQLiteCpp` 现在只由 node 的缓存实现使用，但仍是全项目 configure 必需依赖。
如果未来要做纯 gw/无缓存构建，可增加：

```cmake
option(TQSDK_ENABLE_KLINE_CACHE ...)
```

让 `kline_store.cpp` 和 SQLiteCpp 只在开启时参与。

### 3.6 其他

- `pch.h` 仍包含较多 CLI/Beast 头；可继续拆分 core pch 与 app pch；
- `Date/time` 统一 `TimeStamp=uint64` 的边界，避免 signed/unsigned cast；
- `KlineStore` 目前按每序列 2000 根裁剪，文件 free page 不做 VACUUM；
- `InstrumentCatalog` 目录重复 id 目前 last-wins，可增加数据质量统计；
- Tauri sidecar/动态端口仍未完成（系统 WebKitGTK/rsvg 依赖）。
