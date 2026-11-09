#include "tqsdk/trading/trading_status.h"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace tqsdk {

namespace {

std::tm local_tm(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm           tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::string day_string(const std::tm& tm) {
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m%d");
    return out.str();
}

std::tm add_days(std::tm tm, int days) {
    tm.tm_mday += days;
    tm.tm_isdst = -1;
    std::mktime(&tm);
    return tm;
}

bool is_weekday(const std::tm& tm) {
    return tm.tm_wday != 0 && tm.tm_wday != 6;
}

std::string trading_day_for(const std::tm& tm, TradingCalendar::Phase phase) {
    // 21:00 后的夜盘属于下一交易日；凌晨夜盘沿用当天。
    if (phase == TradingCalendar::Phase::Open && tm.tm_hour >= 21) {
        auto next = add_days(tm, 1);
        while (!is_weekday(next)) {
            next = add_days(next, 1);
        }
        return day_string(next);
    }
    return day_string(tm);
}

std::string session_for(const std::tm& tm, TradingCalendar::Phase phase) {
    if (phase == TradingCalendar::Phase::Closed)
        return "closed";
    if (phase == TradingCalendar::Phase::Settlement)
        return "settlement";
    if (phase == TradingCalendar::Phase::Break)
        return "break";
    if (phase == TradingCalendar::Phase::Unknown)
        return "unknown";
    const int minute = tm.tm_hour * 60 + tm.tm_min;
    if (minute >= 21 * 60 || minute < 3 * 60)
        return "night";
    return "day";
}

TradingStatus make_status(TradingCalendar::Phase phase, std::chrono::system_clock::time_point now) {
    TradingStatus out;
    out.phase               = phase;
    out.phase_name          = std::string(TradingCalendar::name(phase));
    out.trading             = phase == TradingCalendar::Phase::Open;
    out.expected_disconnect = TradingCalendar::is_expected_disconnect(phase);
    const auto tm           = local_tm(now);
    out.trading_day         = trading_day_for(tm, phase);
    out.session             = session_for(tm, phase);
    out.updated_at_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    return out;
}

} // namespace

TradingStatus TradingStatus::from_calendar(const TradingCalendar&                calendar,
                                           std::chrono::system_clock::time_point now) {
    return make_status(calendar.phase_at(now), now);
}

TradingStatus TradingStatus::from_phase(TradingCalendar::Phase                phase,
                                        std::chrono::system_clock::time_point now) {
    if (phase == TradingCalendar::Phase::Unknown) {
        return from_calendar(TradingCalendar{}, now);
    }
    return make_status(phase, now);
}

nlohmann::json TradingStatus::to_json() const {
    return nlohmann::json{
        {"phase", phase_name},
        {"trading", trading},
        {"expected_disconnect", expected_disconnect},
        {"trading_day", trading_day},
        {"session", session},
        {"updated_at_ns", updated_at_ns},
    };
}

} // namespace tqsdk
