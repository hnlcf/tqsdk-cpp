#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {
namespace audit {

struct Options {
    bool        enabled   = false;
    std::string file      = "logs/audit.log";
    std::size_t max_bytes = 10 * 1024 * 1024;
    std::size_t max_files = 5;
};

/// Append-only JSONL audit logger. It never accepts request bodies or
/// credentials; callers only pass event/method/target/status/remote.
class Log {
public:
    explicit Log(Options options);
    ~Log();

    Log(const Log&)            = delete;
    Log& operator=(const Log&) = delete;

    bool enabled() const noexcept { return enabled_; }

    void record(std::string_view event,
                std::string_view method,
                std::string_view target,
                int              status,
                std::string_view remote = {});

private:
    bool                            enabled_ = false;
    std::string                     name_;
    std::shared_ptr<spdlog::logger> logger_;
};

} // namespace audit
} // namespace tqsdk
