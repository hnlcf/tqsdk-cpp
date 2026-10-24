#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {

inline constexpr double  kUnknownDouble = std::numeric_limits<double>::quiet_NaN();
inline constexpr int64_t kUnknownInt    = std::numeric_limits<int64_t>::min();

// 便捷判断
inline bool is_unknown(double v) noexcept {
    return std::isnan(v);
}

inline bool is_unknown(int64_t v) noexcept {
    return v == kUnknownInt;
}

} // namespace tqsdk
