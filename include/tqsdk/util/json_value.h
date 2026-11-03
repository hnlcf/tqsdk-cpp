#pragma once

#include "tqsdk/pch.h"

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <limits>

namespace tqsdk::json_value {

using Json = nlohmann::json;

namespace detail {

inline std::string_view trim_ascii(std::string_view value) {
    auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

template <typename Int>
std::optional<Int> parse_integer(std::string_view text) noexcept {
    text = trim_ascii(text);
    if (text.empty()) {
        return std::nullopt;
    }
    if (text.front() == '+') {
        text.remove_prefix(1);
    } else if constexpr (std::is_unsigned_v<Int>) {
        if (text.front() == '-') {
            return std::nullopt;
        }
    }
    if (text.empty()) {
        return std::nullopt;
    }

    Int value{};
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

inline std::optional<double> parse_double(std::string_view text) noexcept {
    text = trim_ascii(text);
    if (text.empty()) {
        return std::nullopt;
    }
    std::string copy{text};
    char*       end    = nullptr;
    errno              = 0;
    const double value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || errno == ERANGE || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

} // namespace detail

/// JSON number 或严格数字字符串 -> double。拒绝 NaN/Infinity/尾部垃圾。
inline std::optional<double> number(const Json& value) {
    if (value.is_number()) {
        const double result = value.get<double>();
        return std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
    }
    if (value.is_string()) {
        return detail::parse_double(value.get_ref<const std::string&>());
    }
    return std::nullopt;
}

inline double number_or(const Json& object, std::string_view key, double fallback = 0.0) {
    auto it = object.find(key);
    if (it == object.end()) {
        return fallback;
    }
    return number(*it).value_or(fallback);
}

/// JSON 整数或严格整数字符串 -> int64。浮点必须是整数值且在范围内。
inline std::optional<int64_t> integer(const Json& value) {
    if (value.is_number_unsigned()) {
        const auto raw = value.get<uint64_t>();
        if (raw > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<int64_t>(raw);
    }
    if (value.is_number_integer()) {
        return value.get<int64_t>();
    }
    if (value.is_number_float()) {
        const double raw = value.get<double>();
        // 2^63 / 2^64 不能用 double 精确表示，直接使用边界常量，避免
        // static_cast 到整数时越过可表示范围。
        constexpr double kTwo63 = 9'223'372'036'854'775'808.0;
        if (!std::isfinite(raw) || std::trunc(raw) != raw || raw < -kTwo63 || raw >= kTwo63) {
            return std::nullopt;
        }
        return static_cast<int64_t>(raw);
    }
    if (value.is_string()) {
        return detail::parse_integer<int64_t>(value.get_ref<const std::string&>());
    }
    return std::nullopt;
}

inline int64_t integer_or(const Json& object, std::string_view key, int64_t fallback = 0) {
    auto it = object.find(key);
    if (it == object.end()) {
        return fallback;
    }
    return integer(*it).value_or(fallback);
}

/// JSON 非负整数或严格非负整数字符串 -> uint64。
inline std::optional<uint64_t> unsigned_integer(const Json& value) {
    if (value.is_number_unsigned()) {
        return value.get<uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto raw = value.get<int64_t>();
        return raw < 0 ? std::nullopt : std::optional<uint64_t>{static_cast<uint64_t>(raw)};
    }
    if (value.is_number_float()) {
        const double     raw    = value.get<double>();
        constexpr double kTwo64 = 18'446'744'073'709'551'616.0;
        if (!std::isfinite(raw) || std::trunc(raw) != raw || raw < 0.0 || raw >= kTwo64) {
            return std::nullopt;
        }
        return static_cast<uint64_t>(raw);
    }
    if (value.is_string()) {
        const auto text = detail::trim_ascii(value.get_ref<const std::string&>());
        if (text.empty() || text.front() == '-') {
            return std::nullopt;
        }
        return detail::parse_integer<uint64_t>(text);
    }
    return std::nullopt;
}

inline uint64_t
unsigned_integer_or(const Json& object, std::string_view key, uint64_t fallback = 0) {
    auto it = object.find(key);
    if (it == object.end()) {
        return fallback;
    }
    return unsigned_integer(*it).value_or(fallback);
}

/// bool / 0/1 / "true"/"false"/"1"/"0"/"yes"/"no" -> bool。
inline std::string lowercase_ascii(std::string_view text) {
    std::string out{text};
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

inline std::optional<bool> boolean(const Json& value) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_unsigned()) {
        const auto raw = value.get<uint64_t>();
        if (raw == 0 || raw == 1) {
            return raw == 1;
        }
        return std::nullopt;
    }
    if (value.is_number_integer()) {
        const auto raw = value.get<int64_t>();
        if (raw == 0 || raw == 1) {
            return raw == 1;
        }
        return std::nullopt;
    }
    if (value.is_string()) {
        const auto text = lowercase_ascii(detail::trim_ascii(value.get_ref<const std::string&>()));
        if (text == "true" || text == "1" || text == "yes") {
            return true;
        }
        if (text == "false" || text == "0" || text == "no") {
            return false;
        }
    }
    return std::nullopt;
}

inline bool boolean_or(const Json& object, std::string_view key, bool fallback = false) {
    auto it = object.find(key);
    if (it == object.end()) {
        return fallback;
    }
    return boolean(*it).value_or(fallback);
}

/// 只接受 JSON string；不做隐式数字转字符串。
inline std::optional<std::string> string(const Json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    return value.get<std::string>();
}

inline std::string string_or(const Json& object, std::string_view key, std::string fallback) {
    auto it = object.find(key);
    if (it == object.end()) {
        return fallback;
    }
    return string(*it).value_or(std::move(fallback));
}

/// HTTP query/path 等纯文本入口的严格解析。
inline std::optional<uint64_t> parse_unsigned(std::string_view text) noexcept {
    return detail::parse_integer<uint64_t>(text);
}

inline std::optional<bool> parse_boolean(std::string_view text) {
    const auto lower = lowercase_ascii(detail::trim_ascii(text));
    if (lower == "true" || lower == "1" || lower == "yes") {
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "no") {
        return false;
    }
    return std::nullopt;
}

} // namespace tqsdk::json_value
