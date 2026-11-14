#include "tqsdk/ipc/gateway_service.h"

namespace tqsdk {
namespace ipc {

void GatewayServiceRegistry::add(std::unique_ptr<IGatewayService> service) {
    if (!service) {
        return;
    }
    auto name = std::string(service->name());
    if (name.empty()) {
        return;
    }
    services_.insert_or_assign(std::move(name), std::move(service));
}

bool GatewayServiceRegistry::has(std::string_view name) const {
    return services_.contains(std::string(name));
}

asio::awaitable<ServiceResponse> GatewayServiceRegistry::dispatch(const ServiceRequest& request) {
    auto it = services_.find(request.service);
    if (it == services_.end()) {
        co_return ServiceResponse{request.rid,
                                  false,
                                  json::object(),
                                  fmt::format("unknown gateway service '{}'", request.service)};
    }
    auto response = co_await it->second->handle(request);
    response.rid  = request.rid;
    co_return response;
}

} // namespace ipc
} // namespace tqsdk
