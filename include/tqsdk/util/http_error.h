#pragma once

#include "tqsdk/pch.h"

namespace tqsdk::util {

/// 低频 HTTP 客户端的独立错误类别。
///
/// 该类型只属于 util::http_*，不会跨 IPC/gateway/config 层复用。
enum class HttpErrorCode {
    invalid_url,
    resolve,
    connect,
    tls,
    write,
    read,
    http_status,
    response_too_large,
    cancelled,
    unknown,
};

struct HttpError {
    HttpErrorCode code        = HttpErrorCode::unknown;
    int           http_status = 0;
    std::string   detail;
};

constexpr std::string_view http_error_name(HttpErrorCode code) noexcept {
    switch (code) {
    case HttpErrorCode::invalid_url:
        return "invalid_url";
    case HttpErrorCode::resolve:
        return "resolve";
    case HttpErrorCode::connect:
        return "connect";
    case HttpErrorCode::tls:
        return "tls";
    case HttpErrorCode::write:
        return "write";
    case HttpErrorCode::read:
        return "read";
    case HttpErrorCode::http_status:
        return "http_status";
    case HttpErrorCode::response_too_large:
        return "response_too_large";
    case HttpErrorCode::cancelled:
        return "cancelled";
    case HttpErrorCode::unknown:
        return "unknown";
    }
    return "unknown";
}

template <typename T>
using HttpResult = std::expected<T, HttpError>;

} // namespace tqsdk::util
