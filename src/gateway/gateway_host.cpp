#include "tqsdk/gateway/gateway_host.h"

#include "tqsdk/auth/tq_auth.h"
#include "tqsdk/gateway/tq_auth_service.h"

namespace tqsdk {

GatewayHost::GatewayHost(std::unique_ptr<MarketDataGateway> md,
                         std::unique_ptr<TradingGateway>    td,
                         std::shared_ptr<TqAuth>            auth,
                         asio::any_io_executor              ex)
    : md_(std::move(md)),
      td_(std::move(td)) {
    // 即使 TqAuth 未启用也注册服务：node 可以读取 enabled=false 的一致快照，
    // 不需要猜测 gw 是否配置了认证。
    services_.add(std::make_unique<TqAuthService>(std::move(auth), ex));
}

GatewayHost::~GatewayHost() = default;

} // namespace tqsdk
