// include/tqsdk/domain/enum_util.h
#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {

// 不抛异常:非字符串或不在枚举内 → nullopt
template <typename E>
std::optional<E> try_get_enum(const nlohmann::json& v) noexcept {
    if (!v.is_string()) {
        return std::nullopt;
    }
    return magic_enum::enum_cast<E>(v.get_ref<const std::string&>());
}

// 便捷:识别失败返回 fallback
template <typename E>
E get_enum_or(const nlohmann::json& v, E fallback) noexcept {
    return try_get_enum<E>(v).value_or(fallback);
}

} // namespace tqsdk
