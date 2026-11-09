#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/trading/trading_calendar.h"

namespace tqsdk {

/// 面向 API/UI 的交易状态快照。
struct TradingStatus {
    TradingCalendar::Phase phase               = TradingCalendar::Phase::Unknown;
    bool                   trading             = false;
    bool                   expected_disconnect = false;
    std::string            trading_day;
    std::string            session;
    std::string            phase_name;
    std::int64_t           updated_at_ns = 0;

    static TradingStatus
    from_calendar(const TradingCalendar&                calendar,
                  std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    /// 使用网关已知阶段（例如 CTP 的 phase）；未知时回退到 calendar。
    static TradingStatus
    from_phase(TradingCalendar::Phase                phase,
               std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    nlohmann::json to_json() const;
};

} // namespace tqsdk
