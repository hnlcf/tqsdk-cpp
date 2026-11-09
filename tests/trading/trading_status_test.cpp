#include "tqsdk/pch.h"

#include "tqsdk/trading/trading_status.h"

#include <gtest/gtest.h>

namespace {

using namespace tqsdk;

std::chrono::system_clock::time_point local_time(int year, int month, int day, int hour,
                                                 int minute) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon  = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min  = minute;
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    return std::chrono::system_clock::from_time_t(t);
}

TEST(TradingStatus, NightSessionBelongsToNextTradingDay) {
    TradingCalendar::Config cfg;
    cfg.day_sessions = {{0, 24 * 60}};
    cfg.night_sessions.clear();
    cfg.settlement_start_minute = 24 * 60;
    cfg.settlement_end_minute   = 24 * 60;
    cfg.use_global_holidays     = false;
    TradingCalendar calendar(cfg);

    const auto status = TradingStatus::from_calendar(calendar, local_time(2026, 11, 9, 21, 0));
    EXPECT_TRUE(status.trading);
    EXPECT_EQ(status.phase, TradingCalendar::Phase::Open);
    EXPECT_EQ(status.session, "night");
    EXPECT_EQ(status.trading_day, "20261110");
    EXPECT_EQ(status.phase_name, "open");
}

TEST(TradingStatus, ClosedAndSettlementAreExpectedDisconnects) {
    const auto closed = TradingStatus::from_phase(TradingCalendar::Phase::Closed,
                                                  local_time(2026, 11, 9, 8, 0));
    EXPECT_FALSE(closed.trading);
    EXPECT_TRUE(closed.expected_disconnect);
    EXPECT_EQ(closed.session, "closed");

    const auto settlement = TradingStatus::from_phase(TradingCalendar::Phase::Settlement,
                                                      local_time(2026, 11, 9, 16, 0));
    EXPECT_TRUE(settlement.expected_disconnect);
    EXPECT_EQ(settlement.session, "settlement");
    EXPECT_EQ(settlement.to_json()["phase"], "settlement");
}

} // namespace
