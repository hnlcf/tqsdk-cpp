#pragma once

#include "tqsdk/pch.h"

#include <boost/beast/http.hpp>

namespace tqsdk::serve {

/// HTTP/WS 路由层的独立错误类别。
enum class ServeErrorCode {
    bad_request,
    not_found,
    service_unavailable,
    internal,
    invalid_json,
    invalid_path_param,
    invalid_query,
    invalid_body,
    unknown,
};

/// 路由错误同时携带 HTTP 状态和 JSON 响应体，避免上层重新拼错误格式。
struct ServeError {
    boost::beast::http::status status = boost::beast::http::status::internal_server_error;
    nlohmann::json             body   = nlohmann::json::object();
    ServeErrorCode             code   = ServeErrorCode::internal;
};

constexpr std::string_view serve_error_name(ServeErrorCode code) noexcept {
    switch (code) {
    case ServeErrorCode::bad_request:
        return "bad_request";
    case ServeErrorCode::not_found:
        return "not_found";
    case ServeErrorCode::service_unavailable:
        return "service_unavailable";
    case ServeErrorCode::internal:
        return "internal";
    case ServeErrorCode::invalid_json:
        return "invalid_json";
    case ServeErrorCode::invalid_path_param:
        return "invalid_path_param";
    case ServeErrorCode::invalid_query:
        return "invalid_query";
    case ServeErrorCode::invalid_body:
        return "invalid_body";
    case ServeErrorCode::unknown:
        return "unknown";
    }
    return "unknown";
}

template <typename T>
using ServeResult = std::expected<T, ServeError>;

inline std::unexpected<ServeError>
make_serve_error(boost::beast::http::status status,
                 nlohmann::json             body,
                 ServeErrorCode             code = ServeErrorCode::bad_request) noexcept {
    return std::unexpected(ServeError{status, std::move(body), code});
}

} // namespace tqsdk::serve
