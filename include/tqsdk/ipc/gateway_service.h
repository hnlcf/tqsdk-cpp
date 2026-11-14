#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/ipc/protocol.h"

#include <map>
#include <memory>
#include <string_view>

namespace tqsdk {
namespace ipc {

/// GatewayHost 下的可路由服务。md/td 走高频专用帧；其他低频能力
/// （TqAuth、账户查询等）通过 ServiceRequest/ServiceResponse 调用。
class IGatewayService {
public:
    virtual ~IGatewayService() = default;

    virtual std::string_view name() const noexcept = 0;

    /// 在网关控制面调用；实现可在自己的 worker 上完成阻塞工作。
    /// 返回的 rid 由 registry 统一回填。
    virtual asio::awaitable<ServiceResponse> handle(const ServiceRequest& request) = 0;
};

/// 服务名 -> IGatewayService 的路由表。GatewayIpcServer 只依赖该抽象，
/// 不关心 TqAuth/查询等具体组件。
class GatewayServiceRegistry {
public:
    GatewayServiceRegistry()                                         = default;
    GatewayServiceRegistry(const GatewayServiceRegistry&)            = delete;
    GatewayServiceRegistry& operator=(const GatewayServiceRegistry&) = delete;

    void add(std::unique_ptr<IGatewayService> service);
    bool has(std::string_view name) const;

    asio::awaitable<ServiceResponse> dispatch(const ServiceRequest& request);

private:
    std::map<std::string, std::unique_ptr<IGatewayService>> services_;
};

} // namespace ipc
} // namespace tqsdk
