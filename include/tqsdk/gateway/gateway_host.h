#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/gateway/market_data_gateway.h"
#include "tqsdk/gateway/trading_gateway.h"
#include "tqsdk/ipc/gateway_service.h"

namespace tqsdk {

class TqAuth;

/// 顶层接入网关：拥有真实 md/td，并承载可路由的附属服务。
///
///   GatewayHost
///     ├─ MarketDataGateway   (专用高频行情帧)
///     ├─ TradingGateway      (专用交易帧)
///     └─ GatewayServiceRegistry
///           └─ TqAuthService (ServiceRequest/Response)
///
/// 连接层 GatewayIpcServer 只持有 GatewayHost，不直接感知 TqAuth。
class GatewayHost {
public:
    GatewayHost(std::unique_ptr<MarketDataGateway> md,
                std::unique_ptr<TradingGateway>    td,
                std::shared_ptr<TqAuth>            auth,
                asio::any_io_executor              ex);
    ~GatewayHost();

    GatewayHost(const GatewayHost&)            = delete;
    GatewayHost& operator=(const GatewayHost&) = delete;

    MarketDataGateway* md() noexcept { return md_.get(); }
    TradingGateway*    td() noexcept { return td_.get(); }

    ipc::GatewayServiceRegistry&       services() noexcept { return services_; }
    const ipc::GatewayServiceRegistry& services() const noexcept { return services_; }

private:
    std::unique_ptr<MarketDataGateway> md_;
    std::unique_ptr<TradingGateway>    td_;
    ipc::GatewayServiceRegistry        services_;
};

} // namespace tqsdk
