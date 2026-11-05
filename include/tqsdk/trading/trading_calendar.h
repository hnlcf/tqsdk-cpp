#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/trading/holiday_calendar.h"

#include <ctime>
#include <string_view>
#include <utility>
#include <vector>

namespace tqsdk {

/// 交易时段分类。
///
/// CTP 每个交易日结算时都会断开交易前置，这是预期行为，不应与网络故障混为一谈。
/// 本类只提供“当前属于哪个交易阶段”的轻量判断，具体交易所/品种的夜盘差异可在
/// Config 中覆盖；默认值覆盖国内期货常用的日盘/夜盘窗口。
class TradingCalendar {
public:
    enum class Phase {
        Unknown,
        Closed,
        PreOpen,
        Open,
        Break,
        Settlement,
        Reconnecting,
    };

    struct Config {
        /// 日盘交易窗口，单位为“当天 0 点后的分钟数”。
        std::vector<std::pair<int, int>> day_sessions{
            {9 * 60, 10 * 60 + 15},
            {10 * 60 + 30, 11 * 60 + 30},
            {13 * 60 + 30, 15 * 60},
        };
        /// 夜盘交易窗口；默认按 21:00-23:00 的通用窗口。
        std::vector<std::pair<int, int>> night_sessions{
            {21 * 60, 2 * 60 + 30},
        };
        /// 开盘前多少分钟进入 PreOpen。
        int pre_open_minutes = 15;
        /// 日盘收盘后进入 Settlement 的窗口。
        int settlement_start_minute = 15 * 60;
        int settlement_end_minute   = 20 * 60 + 30;

        /// 显式节假日（YYYYMMDD）；测试/自定义交易日历可设置。
        std::vector<int> holidays;
        /// 生产默认使用 HolidayCalendar::global() 的非周末节假日表。
        bool use_global_holidays = true;
    };

    TradingCalendar() = default;
    explicit TradingCalendar(Config cfg)
        : cfg_(std::move(cfg)) {}

    Phase phase_at(std::chrono::system_clock::time_point tp) const {
        const std::time_t t = std::chrono::system_clock::to_time_t(tp);
        std::tm           tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        return phase_at(tm);
    }

    /// 便于单元测试注入固定日期/时间，避免依赖当前系统时钟。
    Phase phase_at(const std::tm& tm) const {
        const int minute = tm.tm_hour * 60 + tm.tm_min;

        // 夜盘跨零点：00:00-夜盘结束属于前一日开始的夜盘。周六凌晨可能是
        // 周五夜盘，因此不能先按“今天是周末”直接 Closed。
        if (is_early_night(minute)) {
            const auto previous = add_days(tm, -1);
            if (is_trading_day_for(previous) && night_session_allowed(previous)) {
                return Phase::Open;
            }
            return Phase::Closed;
        }

        // 非交易日（周末或节假日）的日盘/晚盘全部关闭。
        if (!is_trading_day_for(tm)) {
            return Phase::Closed;
        }
        // 节假日前的夜盘：例如国庆前一日 21:00 不开盘。周五正常周末
        // 不应被关闭；night_session_allowed() 会向后找到下一个交易日，
        // 只有遇到节假日才拒绝。
        const bool night_allowed = night_session_allowed(tm);

        if (in_sessions(minute, cfg_.day_sessions) ||
            (night_allowed && in_sessions(minute, cfg_.night_sessions))) {
            return Phase::Open;
        }
        // 日盘两段之间的休息/午休优先于下一段的 PreOpen；10:15-10:30
        // 这类短休息不应被误报成开盘前集合竞价。
        if (cfg_.day_sessions.size() >= 2 &&
            ((minute >= cfg_.day_sessions.front().second && minute < cfg_.day_sessions[1].first) ||
             (minute >= cfg_.day_sessions[1].second && minute < cfg_.day_sessions.back().first))) {
            return Phase::Break;
        }
        if (minute >= cfg_.settlement_start_minute && minute < cfg_.settlement_end_minute) {
            return Phase::Settlement;
        }
        if (within_pre_open(minute, cfg_.day_sessions) ||
            (night_allowed && within_pre_open(minute, cfg_.night_sessions))) {
            return Phase::PreOpen;
        }
        return Phase::Closed;
    }

    static bool is_trading_day(const std::tm& tm) {
        // tm_wday: 0=Sunday, 6=Saturday. 节假日由实例的 HolidayCalendar 覆盖。
        return tm.tm_wday != 0 && tm.tm_wday != 6;
    }

    static constexpr std::string_view name(Phase phase) noexcept {
        switch (phase) {
        case Phase::Closed:
            return "closed";
        case Phase::PreOpen:
            return "pre_open";
        case Phase::Open:
            return "open";
        case Phase::Break:
            return "break";
        case Phase::Settlement:
            return "settlement";
        case Phase::Reconnecting:
            return "reconnecting";
        case Phase::Unknown:
        default:
            return "unknown";
        }
    }

    static bool is_expected_disconnect(Phase phase) noexcept {
        return phase == Phase::Closed || phase == Phase::Settlement;
    }

    const Config& config() const noexcept { return cfg_; }

private:
    static int date_key(const std::tm& tm) {
        return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
    }

    static std::tm add_days(const std::tm& tm, int days) {
        std::tm out = tm;
        out.tm_mday += days;
        out.tm_isdst = -1;
        std::mktime(&out);
        return out;
    }

    bool is_holiday_date(const std::tm& tm) const {
        const int key = date_key(tm);
        if (std::find(cfg_.holidays.begin(), cfg_.holidays.end(), key) != cfg_.holidays.end()) {
            return true;
        }
        return cfg_.use_global_holidays && HolidayCalendar::global().is_holiday(key);
    }

    bool is_trading_day_for(const std::tm& tm) const {
        return is_trading_day(tm) && !is_holiday_date(tm);
    }

    /// 夜盘属于下一个交易日。正常周五夜盘保留；若下一交易日之前有
    /// 节假日（例如周五后周一放假），当晚必须关闭。
    bool night_session_allowed(const std::tm& day) const {
        if (!is_trading_day_for(day)) {
            return false;
        }
        for (int offset = 1; offset <= 14; ++offset) {
            const auto next = add_days(day, offset);
            if (is_holiday_date(next)) {
                return false;
            }
            if (is_trading_day(next)) {
                return true;
            }
        }
        return false;
    }

    bool is_early_night(int minute) const {
        for (const auto& [begin, end] : cfg_.night_sessions) {
            if (begin > end && minute < end) {
                return true;
            }
        }
        return false;
    }

    bool in_sessions(int minute, const std::vector<std::pair<int, int>>& sessions) const {
        for (const auto& [begin, end] : sessions) {
            if (begin <= end ? (minute >= begin && minute < end)
                             : (minute >= begin || minute < end)) {
                return true;
            }
        }
        return false;
    }

    bool within_pre_open(int minute, const std::vector<std::pair<int, int>>& sessions) const {
        for (const auto& [begin, end] : sessions) {
            (void)end;
            const int pre_begin = begin - cfg_.pre_open_minutes;
            if (pre_begin >= 0 && minute >= pre_begin && minute < begin) {
                return true;
            }
        }
        return false;
    }

    Config cfg_;
};

} // namespace tqsdk
