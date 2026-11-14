#include "tqsdk/auth/remote_auth_service.h"

#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {
namespace auth {

RemoteAuthService::RemoteAuthService(std::shared_ptr<ipc::IpcClient> client,
                                     asio::any_io_executor           ex,
                                     std::chrono::milliseconds       refresh_interval)
    : client_(std::move(client)),
      ex_(std::move(ex)),
      refresh_interval_(refresh_interval),
      timer_(ex_) {}

RemoteAuthService::~RemoteAuthService() {
    stop();
}

asio::awaitable<ipc::IpcResult<void>> RemoteAuthService::refresh() {
    if (!client_) {
        co_return std::unexpected(
            ipc::IpcFailure{ipc::IpcErrorCode::closed, "RemoteAuthService has no IPC client"});
    }
    ipc::ServiceRequest request;
    request.service = "auth";
    request.method  = "snapshot";
    auto response   = co_await client_->request_service(request);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (!response->ok) {
        co_return std::unexpected(ipc::IpcFailure{ipc::IpcErrorCode::protocol, response->error});
    }
    Snapshot    next;
    const auto& result = response->result;
    next.enabled       = result.value("enabled", false);
    next.logged_in     = result.value("logged_in", false);
    next.user_name     = result.value("user_name", std::string{});
    next.user_agent    = result.value("user_agent", std::string{});
    next.authorization = result.value("authorization", std::string{});
    next.auth_id       = result.value("auth_id", std::string{});
    next.product_type  = result.value("product_type", std::string{});
    next.grants        = result.value("grants", std::string{});
    next.last_error    = result.value("last_error", std::string{});
    next.expire_days   = result.value("expire_days", 0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = std::move(next);
    }
    co_return ipc::IpcResult<void>{};
}

void RemoteAuthService::start() {
    if (started_ || stopped_) {
        return;
    }
    started_  = true;
    auto self = shared_from_this();
    co_spawn_logged(
        ex_,
        [self]() -> asio::awaitable<void> { co_await self->refresh_loop(); },
        "Node.remote_auth.refresh");
}

void RemoteAuthService::stop() noexcept {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    timer_.cancel();
}

RemoteAuthService::Snapshot RemoteAuthService::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

std::string RemoteAuthService::user_agent() const {
    auto snap = snapshot();
    return snap.user_agent;
}

std::string RemoteAuthService::authorization_header() {
    auto snap = snapshot();
    return snap.authorization;
}

asio::awaitable<void> RemoteAuthService::refresh_loop() {
    while (!stopped_) {
        timer_.expires_after(refresh_interval_);
        boost::system::error_code ec;
        co_await timer_.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        if (stopped_ || ec == asio::error::operation_aborted) {
            co_return;
        }
        auto result = co_await refresh();
        if (!result) {
            SPDLOG_DEBUG("[RemoteAuth] refresh failed: {} ({})",
                         result.error().detail,
                         ipc::ipc_error_name(result.error().code));
        }
    }
}

} // namespace auth
} // namespace tqsdk
