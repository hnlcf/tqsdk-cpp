#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {

constexpr const char* kLogLevelEnv = "TQSDK_LOG_LEVEL";

/// 日志初始化选项。
struct LogOptions {
    /// 日志级别名（trace/debug/info/warn/error/critical/off）。
    /// 为空时读取环境变量 TQSDK_LOG_LEVEL，再缺省为 "info"。
    std::string level;

    /// 日志模式。默认带时间戳、级别、线程号、来源与正文。
    std::string pattern = "%Y-%m-%d %H:%M:%S.%e [%^%l%$] [%s:%#] [t:%t] : %v";

    /// 控制台输出是否着色（默认开启；重定向到文件时应关闭）。
    bool colored = true;

    /// 可选：同时把日志写入该文件路径（为空则不写文件）。
    std::string file;
};

/// 从字符串解析 spdlog 级别；无法识别时回退到 info 并返回 false。
std::optional<spdlog::level::level_enum> parse_log_level(std::string_view name) noexcept;

/// 初始化全局默认 spdlog logger。
///
/// 行为：
///   - 级别优先级：options.level > 环境变量 TQSDK_LOG_LEVEL > "info"。
///   - 默认输出到 stdout（着色），若 options.file 非空则额外挂载一个
///     滚动文件 sink（>1MB 轮转，保留 3 个备份）。
///   - 幂等：可重复调用，最后一次生效（会替换默认 logger）。
///
/// 进程内任何使用 spdlog::info/warn/... 的代码（含库内部）都会走这套配置。
void setup_logging(const LogOptions& options = {});

} // namespace tqsdk
