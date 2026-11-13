#pragma once

#include "tqsdk/pch.h"

namespace tqsdk::gateway {

/// Gateway/TqApi 构造边界的独立错误类别。
enum class GatewayErrorCode {
    config,
    connect,
    handshake,
    protocol,
    timeout,
    auth,
    unknown,
};

struct GatewayError {
    GatewayErrorCode code = GatewayErrorCode::unknown;
    std::string      detail;
};

constexpr std::string_view gateway_error_name(GatewayErrorCode code) noexcept {
    switch (code) {
    case GatewayErrorCode::config:
        return "config";
    case GatewayErrorCode::connect:
        return "connect";
    case GatewayErrorCode::handshake:
        return "handshake";
    case GatewayErrorCode::protocol:
        return "protocol";
    case GatewayErrorCode::timeout:
        return "timeout";
    case GatewayErrorCode::auth:
        return "auth";
    case GatewayErrorCode::unknown:
        return "unknown";
    }
    return "unknown";
}

template <typename T>
using GatewayResult = std::expected<T, GatewayError>;

} // namespace tqsdk::gateway
