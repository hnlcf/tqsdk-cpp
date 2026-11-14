#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/ipc/gateway_service.h"

#include <boost/asio/thread_pool.hpp>

namespace tqsdk {

class TqAuth;

/// 把 TqAuth 暴露成网关服务。TqAuth 的登录/刷新/名称服务是同步 HTTP，
/// 因此放到独立 worker 线程执行，避免阻塞 IPC 控制面。
class TqAuthService final : public ipc::IGatewayService {
public:
    TqAuthService(std::shared_ptr<TqAuth> auth, asio::any_io_executor ex);
    ~TqAuthService() override;

    TqAuthService(const TqAuthService&)            = delete;
    TqAuthService& operator=(const TqAuthService&) = delete;

    std::string_view name() const noexcept override { return "auth"; }

    asio::awaitable<ipc::ServiceResponse> handle(const ipc::ServiceRequest& request) override;

private:
    ipc::ServiceResponse handle_sync(const ipc::ServiceRequest& request);

    std::shared_ptr<TqAuth> auth_;
    asio::thread_pool       worker_{1};
};

} // namespace tqsdk
