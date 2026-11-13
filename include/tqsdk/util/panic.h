#pragma once

#include "tqsdk/pch.h"

#include <source_location>

namespace tqsdk::detail {

[[noreturn]] inline void
panic_at(std::string_view     message,
         std::source_location loc = std::source_location::current()) noexcept {
    try {
        spdlog::critical(
            "[panic] {}:{} {}: {}", loc.file_name(), loc.line(), loc.function_name(), message);
        spdlog::default_logger()->flush();
    } catch (...) {
    }
    std::abort();
}

template <typename... Args>
std::string format_panic(fmt::format_string<Args...> format, Args&&... args) noexcept {
    try {
        return fmt::format(format, std::forward<Args>(args)...);
    } catch (...) {
        return "panic formatting failed";
    }
}

} // namespace tqsdk::detail

/// 不可恢复的编程错误/不变量破坏。
///
/// 使用宏而不是普通函数，才能由 `std::source_location::current()` 捕获
/// 真正的调用文件、函数和行号。只在“继续运行会破坏账户、协议或内存安全”时使用。
#define TQSDK_PANIC(...)                                                                           \
    ::tqsdk::detail::panic_at(::tqsdk::detail::format_panic(__VA_ARGS__),                          \
                              std::source_location::current())

namespace tqsdk {

[[noreturn]] inline void panic(std::string_view message) noexcept {
    detail::panic_at(message, std::source_location::current());
}

} // namespace tqsdk
