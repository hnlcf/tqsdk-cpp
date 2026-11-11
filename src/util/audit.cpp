#include "tqsdk/util/audit.h"

#include "tqsdk/util/log.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace tqsdk {
namespace audit {

namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

Log::Log(Options options)
    : enabled_(options.enabled) {
    if (!enabled_)
        return;
    name_ = "tqsdk.audit." + std::to_string(reinterpret_cast<std::uintptr_t>(this));
    if (options.file.empty()) {
        auto sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
        sink->set_pattern("%v");
        logger_ = std::make_shared<spdlog::logger>(name_, std::move(sink));
    } else {
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            options.file,
            std::max<std::size_t>(1024, options.max_bytes),
            std::max<std::size_t>(1, options.max_files));
        sink->set_pattern("%v");
        logger_ = std::make_shared<spdlog::logger>(name_, std::move(sink));
    }
    logger_->set_level(spdlog::level::info);
    logger_->flush_on(spdlog::level::info);
}

Log::~Log() {
    if (!name_.empty()) {
        if (logger_)
            logger_->flush();
        spdlog::drop(name_);
    }
}

void Log::record(std::string_view event,
                 std::string_view method,
                 std::string_view target,
                 int              status,
                 std::string_view remote) {
    if (!enabled_ || !logger_)
        return;
    const std::string safe_target = redact_secrets(target);
    nlohmann::json    row{
        {"ts", now_ms()},
        {"event", event},
        {"method", method},
        {"target", safe_target},
        {"status", status},
        {"remote", remote},
    };
    try {
        logger_->info("{}", row.dump());
    } catch (...) {
        // Audit must never break request handling.
    }
}

} // namespace audit
} // namespace tqsdk
