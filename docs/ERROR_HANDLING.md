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
| `ipc` | `ipc::IpcFailure` / `IpcErrorCode` | 建连/握手边界；帧内部仍使用 IpcError 适配 |
| `gateway` | `gateway::GatewayError` / `GatewayErrorCode` | TqApi 建连/握手、网关构造边界 |
| `auth` | `auth::AuthError` / `AuthErrorCode` | 快期账户登录边界 |
| `data` | `bool ready()` / status JSON | 缓存打开失败降级；数据读写不在公共 API 抛异常 |
| `serve` | `serve::ServeError` | 参数提取、路由、HTTP 映射 |
| `backtest` | `tq_bt_stats::status` | C ABI 内部 catch，返回 status/error 字符串 |
| `app` | exit code + ERROR 日志 | 进程入口的最终汇总，不向调用方抛异常 |

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
- `IpcClient::connect()` / `send_command_and_wait()` 返回 `expected<..., IpcFailure>`；
- `TqAuth::login_result()` 返回 `expected<void, AuthError>`；
- HTTP route handler 返回 `ServeResult<json>`，router 在边界解包为 Response；
- `node` 主账户和附加账户均使用 expected 边界；
- HTTP 调用方（holiday/catalog/continuous/auth）已在本层处理错误；
- 新增 `TQSDK_PANIC(...)`（记录 file/line/function）及独立模块错误类型；
- 新增错误路径测试。

### 6.1 内部异常白名单

公共边界已改为 expected/错误码后，以下内部路径仍允许异常，但它们必须在模块边界
被捕获，不允许穿透到 app/HTTP/IPC 对端：

- Boost.Asio 协程的 `operation_aborted`/取消异常：作为协程控制流保留；
- nlohmann JSON、toml++、SQLiteCpp、OpenSSL 的适配器异常：在 codec/config/store/http
  适配层捕获并转换；
- `WsProtocolError`、`IpcError`：在 WebSocket/IPC session 边界转换为错误事件或关闭连接；
- `config` 的旧 `parse_*` 兼容 API：内部统一由 `try_parse_*`/`load_*` 包装；
- CTP/SIM 网关的异步内部路径：由 `try_make_tqapi`/事件循环边界转换为 GatewayError。

这些内部异常不再跨进程或跨公共 API。后续如要把这些路径也完全改成
`std::expected`，应逐模块替换 Asio `use_awaitable` 的异常接口，而不是继续增加
跨层 throw。
