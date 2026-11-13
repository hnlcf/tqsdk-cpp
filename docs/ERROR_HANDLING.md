# 错误处理与日志规范

本文定义 TQSDK C++ 的错误、异常隔离、日志和边界处理规则。目标是让错误在
**最近的正确层次**被处理，而不是从底层一路抛到进程入口。

当前语言标准：C++23，使用 `std::expected<T, E>`。不引入项目级统一 `Error` 类；
每个独立模块拥有自己的错误码和错误载荷。

## 1. 模块错误所有权

| 模块 | 错误类型 | 适用范围 |
|---|---|---|
| `util/http_*` | `util::HttpError` / `HttpErrorCode` | URL、DNS、连接、TLS、HTTP 状态、读写 |
| `config` | `config::ConfigError` / `ConfigErrorCode` | TOML 文件、语法、字段校验 |
| `ipc` | `ipc::IpcError`（待迁移为 expected） | 帧、握手、会话、重连 |
| `gateway` | `gateway::GatewayError` / `GatewayErrorCode` | TqApi 建连/握手、网关构造边界 |
| `data` | `data::DataError`（待添加） | 缓存、持久化、反序列化、数据一致性 |
| `serve` | `serve::ServeError`（待添加） | 参数提取、路由、HTTP 映射 |
| `backtest` | `backtest::BacktestError`（待添加） | C ABI 参数、回测引擎输入 |
| `app` | `app::AppError`（待添加） | 进程入口的最终错误汇总 |

禁止把 `HttpError` 直接暴露到 IPC/gateway 上层；上层应记录该错误并转换为本层
错误码。错误载荷应该小而稳定：错误码 + 必要上下文，不携带异常对象、连接对象
或敏感信息。

## 2. 边界与处理策略

### 2.1 可恢复错误：返回 expected

适合：

- 配置读取/解析；
- HTTP 下载；
- 行情缺失、缓存未命中、合约不存在；
- 单次 IPC 帧/握手失败；
- 用户输入/参数校验；
- 单次报单被柜台拒绝。

函数形态：

```cpp
std::expected<T, ModuleError> operation(...) noexcept;
```

调用方 **必须** 在最近的边界处理：

```cpp
auto result = load_config(path);
if (!result) {
    SPDLOG_ERROR("config failed: {} ({})",
                 result.error().detail,
                 config::config_error_name(result.error().code));
    return Failure;
}
```

### 2.2 任务级失败：记录日志后终止当前连接/任务

适合：

- 某个 IPC 连接握手失败；
- 某次 HTTP 刷新失败但后台线程继续；
- 某个 WebSocket 客户端发送非法帧；
- 单个账户连接失败。

处理方式：本层记录错误、关闭该连接/任务，不让错误越过账户或会话边界。

### 2.3 不可恢复：panic

使用 `tqsdk/panic.h` 的 `TQSDK_PANIC("reason {}", value)` 宏（记录文件/函数/行号）：

- 继续运行会破坏账户、订单、协议或内存安全；
- 已满足前置条件后，内部状态机进入不可能状态；
- 持久化文件成功解析但关键不变量被破坏。

`TQSDK_PANIC` 会输出 `file:line function: message` 后 `std::abort()`；禁止用 panic 处理网络错误、配置错误、普通输入错误或第三方异常。

## 3. 第三方库异常隔离

第三方库（toml++、nlohmann::json、SQLiteCpp、Boost.Asio/Beast、OpenSSL）
可能抛异常。要求：

1. 第三方调用集中在适配函数/适配类中；
2. 适配函数捕获 `std::exception` / `...`，转换为本模块 error；
3. 不允许第三方异常穿透到 TqApi、IPC、HTTP 或 app 边界；
4. 构造函数不能返回 expected 时，提供 `create()` 静态工厂或 `init()` 返回
   expected；构造失败不得留下半初始化对象。

C ABI（`backtest_c_api.cpp`）例外：异常绝不允许跨 C ABI，必须在边界处捕获并
转换成 C 错误码/错误字符串。

## 4. noexcept 规则

- 不分配、不调用第三方、只做状态判断/算术的函数：`noexcept`；
- 返回 expected 但内部可能分配错误字符串的函数：可以 `noexcept`，但必须在
  边界捕获所有异常；若 `bad_alloc` 无法安全构造错误，允许终止；
- 协程：`asio::awaitable<T>` 不标 `noexcept`；在协程入口/出口把异常转换为
  expected 或记录后终止当前任务；
- 析构函数：`noexcept`，不得抛异常。

## 5. 日志规则

- 错误边界必须记录：模块、操作、错误码、上下文；不得记录密码、token、完整
  账户号；
- 同一次错误只在一个边界记录一次，避免每层重复刷屏；
- 可预期的高频事件（行情缺失、缓存 miss）用 DEBUG/TRACE；
- 连接断开、重连、报单拒绝、配置失败用 WARN/ERROR；
- 进程入口只记录最终摘要和退出码。

## 6. 当前迁移状态

已完成：

- `util::http_get/http_put/http_post_form` → `std::expected<std::string, HttpError>`
  并捕获 Boost/Beast/SSL 异常；
- `config::load_gw_config/load_node_config/load_tqapi_config` 及
  `try_parse_*` → `ConfigResult<T>`；
- `gw_main/node_main` 不再用异常捕获配置错误，改为 expected 分支；
- 新增 `try_make_tqapi()`，把 gateway 初始化异常隔离为 `GatewayError`；
- `node` 主账户和附加账户均使用 expected 边界；
- HTTP 调用方（holiday/catalog/continuous/auth）已在本层处理错误；
- 新增 `util::panic` 及 HTTP/config 独立错误类型；
- 新增错误路径测试。

后续迁移：

- `ipc::IpcError` 从 `runtime_error` 改为帧/会话 expected；
- `serve::HttpError` 从 throw 改为 handler `expected<Response, ServeError>`；
- gateway 内部 TQ/CTP/Sim 异步操作继续向 GatewayError 细化；
- `data` 的 SQLite/JSON 适配器统一 expected；
- `backtest_c_api` 增加 C 错误码并清理所有 catch。
