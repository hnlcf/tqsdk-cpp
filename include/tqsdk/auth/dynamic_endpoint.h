#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {

class TqAuth;

struct Endpoint {
    std::string   scheme;
    std::string   host;
    std::uint16_t port = 0;
    std::string   path;

    static std::optional<Endpoint> parse(std::string_view url);
    std::string                    to_string() const;
};

/// 把 TqAuth 的名称服务地址转换为可配置的 MD/TD 端点。
///
/// MD：`wss://host/path` -> TqMdGateway 需要的 host/port/path；
/// TD：保留原始 URL（Tq 交易网关协议接入点），供后续 TqTdGateway 使用。
class DynamicEndpointResolver {
public:
    explicit DynamicEndpointResolver(TqAuth& auth)
        : auth_(auth) {}

    std::optional<Endpoint>    md(bool stock = false, bool backtest = false);
    std::optional<std::string> td(std::string_view broker_name, std::string_view account_id);

private:
    TqAuth& auth_;
};

} // namespace tqsdk
