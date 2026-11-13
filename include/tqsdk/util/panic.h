#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {

/// 不可恢复的编程错误/不变量破坏。
///
/// 使用场景仅限“继续运行会破坏账户/协议/内存安全”的情况，例如：
/// - 已成功登录后核心状态机进入不可能的 phase；
/// - 持久化文件已成功解析但关键字段违反内部不变量；
/// - 公共 API 被以违反前置条件的方式调用。
///
/// 网络错误、配置错误、第三方异常、输入错误都不应使用 panic；这些应返回
/// 各模块自己的 expected/error 或在上报日志后终止当前任务。
[[noreturn]] inline void panic(std::string_view message) noexcept {
    try {
        spdlog::critical("[panic] {}", message);
        spdlog::default_logger()->flush();
    } catch (...) {
    }
    std::abort();
}

} // namespace tqsdk
