#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/auth/auth_token_source.h"
#include "tqsdk/ipc/ipc_client.h"

namespace tqsdk {
namespace auth {

/// node 侧的 TqAuth 只读代理。
///
/// node 不持有 TqAuth，也不接触账号密码；它通过同一条 IPC 连接向
/// GatewayHost 的 auth service 请求快照，并缓存 Bearer token/user-agent
/// 供 InstrumentCatalog 的同步下载线程使用。
class RemoteAuthService : public AuthTokenSource,
                          public std::enable_shared_from_this<RemoteAuthService> {
public:
    struct Snapshot {
        bool        enabled   = false;
        bool        logged_in = false;
        std::string user_name;
        std::string user_agent;
        std::string authorization;
        std::string auth_id;
        std::string product_type;
        std::string grants;
        std::string last_error;
        int         expire_days = 0;
    };

    RemoteAuthService(std::shared_ptr<ipc::IpcClient> client,
                      asio::any_io_executor           ex,
                      std::chrono::milliseconds       refresh_interval = std::chrono::seconds(60));
    ~RemoteAuthService() override;

    RemoteAuthService(const RemoteAuthService&)            = delete;
    RemoteAuthService& operator=(const RemoteAuthService&) = delete;

    /// 主动请求一次 auth snapshot；登录/刷新由 gw 侧 TqAuth 完成。
    asio::awaitable<ipc::IpcResult<void>> refresh();

    /// 启动周期刷新；须在 shared_ptr 生命周期内调用。
    void start();
    void stop() noexcept;

    Snapshot snapshot() const;

    std::string user_agent() const override;
    std::string authorization_header() override;

private:
    asio::awaitable<void> refresh_loop();

    std::shared_ptr<ipc::IpcClient> client_;
    asio::any_io_executor           ex_;
    std::chrono::milliseconds       refresh_interval_;
    asio::steady_timer              timer_;
    mutable std::mutex              mutex_;
    Snapshot                        snapshot_;
    bool                            started_ = false;
    bool                            stopped_ = false;
};

} // namespace auth
} // namespace tqsdk
