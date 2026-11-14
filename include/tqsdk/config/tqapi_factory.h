#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/auth/remote_auth_service.h"
#include "tqsdk/config/config_error.h"
#include "tqsdk/config/model.h"
#include "tqsdk/gateway/gateway_error.h"
#include "tqsdk/tqapi.h"

namespace tqsdk {

/// 用一组 IPC 端点构造 node 侧 Remote 引擎配置。
EngineConfig make_remote_engine_config(const IpcConfig& ipc, std::string account_id);

/// 远程引擎 + 同一条 IPC 上的附属服务。
struct EngineRuntime {
    std::unique_ptr<TqApi>                   api;
    std::shared_ptr<auth::RemoteAuthService> auth;
};

/// 依据运行时引擎配置构造一个 TqApi。
///
/// 对进程内网关（tq/sim）同步构造；对远程网关需先建立 IPC 连接并完成握手，
/// 因此本函数是协程。返回的 TqApi 已 start()，可直接驱动 io.run()。
///
/// 远程模式下 md/td 共用 `cfg.ipc` 描述的同一条 IpcSession。
asio::awaitable<gateway::GatewayResult<std::unique_ptr<TqApi>>>
make_tqapi(asio::io_context&           ctx,
           const EngineConfig&         cfg,
           asio::any_io_executor       core_ex     = {},
           std::shared_ptr<KlineStore> kline_store = nullptr) noexcept;

/// 与 make_tqapi 相同，但把建连/握手/配置异常隔离为本层 GatewayError。
asio::awaitable<gateway::GatewayResult<std::unique_ptr<TqApi>>>
try_make_tqapi(asio::io_context&           ctx,
               const EngineConfig&         cfg,
               asio::any_io_executor       core_ex     = {},
               std::shared_ptr<KlineStore> kline_store = nullptr) noexcept;

/// 与 make_tqapi 相同，但额外返回共享 IPC 上的 RemoteAuthService。
/// node 用它读取 GatewayHost 的 TqAuth 快照，不需要持有 TqAuth。
asio::awaitable<gateway::GatewayResult<EngineRuntime>>
make_engine_runtime(asio::io_context&           ctx,
                    const EngineConfig&         cfg,
                    asio::any_io_executor       core_ex     = {},
                    std::shared_ptr<KlineStore> kline_store = nullptr) noexcept;

/// 解析 gw 配置（TOML 文本；IPC 组需内联在 [ipc.<key>]）。
config::ConfigResult<GatewayConfig> parse_gw_config(std::string_view toml_text) noexcept;
config::ConfigResult<GatewayConfig> try_parse_gw_config(std::string_view toml_text) noexcept;

/// 解析 node 配置（TOML 文本；账户 IPC 组需内联）。
config::ConfigResult<NodeConfig> parse_node_config(std::string_view toml_text) noexcept;
config::ConfigResult<NodeConfig> try_parse_node_config(std::string_view toml_text) noexcept;

/// 从目录或单文件加载；IPC 组优先从 `<config>/ipc/<key>.toml` 解析。
config::ConfigResult<GatewayConfig> load_gw_config(const std::string& path) noexcept;
config::ConfigResult<NodeConfig>    load_node_config(const std::string& path) noexcept;

} // namespace tqsdk
