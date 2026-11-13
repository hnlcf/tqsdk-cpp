#pragma once

#include "tqsdk/pch.h"

namespace tqsdk::auth {

/// 快期账户认证的独立错误类别。
enum class AuthErrorCode {
    disabled,
    missing_credentials,
    transport,
    invalid_response,
    unsupported_broker,
    unknown,
};

struct AuthError {
    AuthErrorCode code = AuthErrorCode::unknown;
    std::string   detail;
};

constexpr std::string_view auth_error_name(AuthErrorCode code) noexcept {
    switch (code) {
    case AuthErrorCode::disabled:
        return "disabled";
    case AuthErrorCode::missing_credentials:
        return "missing_credentials";
    case AuthErrorCode::transport:
        return "transport";
    case AuthErrorCode::invalid_response:
        return "invalid_response";
    case AuthErrorCode::unsupported_broker:
        return "unsupported_broker";
    case AuthErrorCode::unknown:
        return "unknown";
    }
    return "unknown";
}

template <typename T>
using AuthResult = std::expected<T, AuthError>;

} // namespace tqsdk::auth
