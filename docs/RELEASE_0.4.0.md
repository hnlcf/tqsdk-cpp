# TQSDK C++ 0.4.0 Release Notes

## 主题

这一版的主题是 **错误处理边界重构** 和 **C++23 构建链收口**。

## 主要变化

### 错误处理

- 公共边界统一使用 `std::expected`：
  - HTTP：`HttpResult<T>`
  - config：`ConfigResult<T>`
  - IPC：`IpcResult<T>`
  - gateway：`GatewayResult<T>`
  - auth：`AuthResult<T>`
  - serve：`ServeResult<T>`
  - WS 解析：`WsParseResult<T>`
- 删除全部项目自建异常类：
  - `IpcError`
  - `WsProtocolError`
  - `AccountNotFound`
- IPC 通道/会话/客户端改造：
  - `IIpcChannel::async_send/async_recv`
  - `IpcSession::start/send/send_body`
  - `IpcClient::connect/subscribe/send_resume/send_command_and_wait`
- config 解析改为内部错误状态汇总，公共入口返回 `ConfigResult`；
- HTTP route 参数提取改为 `ServeResult`，router 不再抛 `HttpError`；
- `HttpServer::listen` 返回 `ServeResult<void>`；
- CTP 停机、gateway 启动失败、holiday/catalog 校验改为日志/错误返回。

### 构建与可维护性

- C++23 `std::expected` 构建在 GCC 14/16 上验证；
- GitHub CI/Release workflow 改用 `g++-14`，避免 clang 18 与 libstdc++ 头文件混用；
- `.clang-format` 使用 C++23 + `InsertBraces: true`；
- 代码路径中的字符串拼接统一改为 `fmt::format`；
- `TQSDK_PANIC(...)` 使用 `std::source_location`，输出文件、函数、行号。

## API 兼容性

这是 0.4.0 的 breaking change：

```cpp
// 旧
auto api = co_await make_tqapi(ctx, cfg);

// 新
auto result = co_await make_tqapi(ctx, cfg);
if (!result) {
    // result.error()
}
auto api = std::move(*result);
```

同样需要调整：

- `IpcClient::connect`
- `IpcSession::start/send`
- `HttpServer::listen`
- `parse_*_config`
- WS parser

## 验证

- CTest：365/365（2 项网络测试按环境跳过）
- Python tests：21/21
- GCC 本地构建：通过
- `tqsdk-node --help`：0

## 已知限制

- 内部仍有 46 处 `throw`，均为 nlohmann `from_json`、IPC 序列化适配器或 Asio
  `operation_aborted` 取消协议；公共 API 不再暴露这些异常。
- 真实 CTP 连续 5 个交易日矩阵仍需自然交易日时间。
- 7 天 CTP soak 需要基于 0.4.0 最终构建重新执行。
- Windows CTP 尚未验证。
- SimNow 双账户真实柜台验收仍需可用双账户配置。
