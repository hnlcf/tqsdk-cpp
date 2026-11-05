#pragma once

#include "tqsdk/pch.h"

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
            {21 * 60, 23 * 60},
        };
        /// 开盘前多少分钟进入 PreOpen。
        int pre_open_minutes = 15;
        /// 日盘收盘后进入 Settlement 的窗口。
        int settlement_start_minute = 15 * 60;
        int settlement_end_minute   = 20 * 60 + 30;
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
        if (!is_trading_day(tm)) {
            return Phase::Closed;
        }
        const int minute = tm.tm_hour * 60 + tm.tm_min;
        if (in_sessions(minute, cfg_.day_sessions) || in_sessions(minute, cfg_.night_sessions)) {
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
        if (within_pre_open(minute)) {
            return Phase::PreOpen;
        }
        return Phase::Closed;
    }

    static bool is_trading_day(const std::tm& tm) {
        // tm_wday: 0=Sunday, 6=Saturday. 节假日需要上层用交易日历覆盖。
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
    bool in_sessions(int minute, const std::vector<std::pair<int, int>>& sessions) const {
        for (const auto& [begin, end] : sessions) {
            if (begin <= end ? (minute >= begin && minute < end)
                             : (minute >= begin || minute < end)) {
                return true;
            }
        }
        return false;
    }

    bool within_pre_open(int minute) const {
        for (const auto& sessions : {cfg_.day_sessions, cfg_.night_sessions}) {
            for (const auto& [begin, end] : sessions) {
                (void)end;
                const int pre_begin = begin - cfg_.pre_open_minutes;
                if (pre_begin >= 0 && minute >= pre_begin && minute < begin) {
                    return true;
                }
            }
        }
        return false;
    }

    Config cfg_;
};

} // namespace tqsdk
