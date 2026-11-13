#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/util/json_value.h"

namespace tqsdk {
namespace serve {
namespace detail {

inline std::size_t query_size(std::string_view value, std::size_t fallback, std::size_t max_value) {
    if (value.empty()) {
        return fallback;
    }
    const auto parsed = json_value::parse_unsigned(value);
    return parsed ? std::min<std::size_t>(max_value, static_cast<std::size_t>(*parsed)) : fallback;
}

inline bool query_bool(std::string_view value, bool fallback) {
    if (value.empty()) {
        return fallback;
    }
    return json_value::parse_boolean(value).value_or(fallback);
}

inline nlohmann::json catalog_not_ready() {
    return nlohmann::json{{"error", "instrument catalog is not loaded"}};
}

} // namespace detail
} // namespace serve
} // namespace tqsdk
