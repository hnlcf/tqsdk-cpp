#include "tqsdk/util/log.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <cstdlib>

namespace tqsdk {

std::optional<spdlog::level::level_enum> parse_log_level(std::string_view name) noexcept {
    // spdlog::level::from_str 对未知返回 off，无法区分；这里自己查表。
    static const std::pair<std::string_view, spdlog::level::level_enum> kTable[] = {
        {"trace", spdlog::level::trace},
        {"debug", spdlog::level::debug},
        {"info", spdlog::level::info},
        {"warn", spdlog::level::warn},
        {"warning", spdlog::level::warn},
        {"error", spdlog::level::err},
        {"critical", spdlog::level::critical},
        {"off", spdlog::level::off},
    };
    for (const auto& [k, v] : kTable) {
        if (k == name) {
            return v;
        }
    }
    return std::nullopt;
}

void setup_logging(const LogOptions& options) {
    // 1) 决定级别：options.level > env(TQSDK_LOG_LEVEL) > info
    spdlog::level::level_enum level = spdlog::level::info;
    if (!options.level.empty()) {
        level = parse_log_level(options.level).value_or(spdlog::level::info);
    } else if (const char* env = std::getenv(kLogLevelEnv); env != nullptr) {
        // 直接从环境变量解析，随后应用到新建的默认 logger 上。
        // （不能只靠 spdlog::cfg::load_env_levels：它在下方替换默认 logger 前调用，
        //   级别会落在旧 logger 上而丢失。）
        if (auto lv = parse_log_level(env)) {
            level = *lv;
        }
    }

    // 2) 控制台 sink（着色或朴素，取决于 colored 与是否 tty）。
    std::shared_ptr<spdlog::sinks::sink> console_sink;
    if (options.colored) {
        console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    } else {
        console_sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
    }
    console_sink->set_pattern(options.pattern);

    std::vector<spdlog::sink_ptr> sinks{console_sink};

    // 3) 可选：滚动文件 sink。
    if (!options.file.empty()) {
        constexpr std::size_t kMaxBytes = 1024 * 1024; // 1 MB
        constexpr std::size_t kMaxFiles = 3;
        auto                  file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            options.file, kMaxBytes, kMaxFiles);
        file_sink->set_pattern(options.pattern);
        sinks.push_back(file_sink);
    }

    // 4) 组装 multipart logger 并设为默认。
    auto logger = std::make_shared<spdlog::logger>("tqsdk", sinks.begin(), sinks.end());
    logger->set_level(level);
    logger->flush_on(spdlog::level::warn); // warn 及以上立即刷盘，便于崩溃前留痕
    spdlog::set_default_logger(logger);

    // 5) 库内 err 及以上即时刷新，避免丢日志。
    spdlog::flush_on(spdlog::level::err);

    SPDLOG_DEBUG("[log] logging initialized (level={})", spdlog::level::to_string_view(level));
}

} // namespace tqsdk
