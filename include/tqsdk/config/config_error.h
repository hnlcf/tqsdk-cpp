#pragma once

#include "tqsdk/pch.h"

namespace tqsdk::config {

/// 配置层的独立错误类别。
enum class ConfigErrorCode {
    file_io,
    parse,
    invalid_value,
    unsupported,
    unknown,
};

struct ConfigError {
    ConfigErrorCode code = ConfigErrorCode::unknown;
    std::string     detail;
};

constexpr std::string_view config_error_name(ConfigErrorCode code) noexcept {
    switch (code) {
    case ConfigErrorCode::file_io:
        return "file_io";
    case ConfigErrorCode::parse:
        return "parse";
    case ConfigErrorCode::invalid_value:
        return "invalid_value";
    case ConfigErrorCode::unsupported:
        return "unsupported";
    case ConfigErrorCode::unknown:
        return "unknown";
    }
    return "unknown";
}

template <typename T>
using ConfigResult = std::expected<T, ConfigError>;

} // namespace tqsdk::config
