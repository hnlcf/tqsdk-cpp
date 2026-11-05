#include "tqsdk/pch.h"

#include "tqsdk/trading/trading_calendar.h"

#include <ctime>

namespace {

std::tm make_tm(int year, int month, int day, int hour, int minute) {
    std::tm tm{};
    tm.tm_year  = year - 1900;
    tm.tm_mon   = month - 1;
    tm.tm_mday  = day;
    tm.tm_hour  = hour;
    tm.tm_min   = minute;
    tm.tm_isdst = -1;
    std::mktime(&tm); // 填充 tm_wday
    return tm;
}

} // namespace

TEST(TradingCalendar, WeekdayPhases) {
    tqsdk::TradingCalendar calendar;
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 5, 8, 50)),
              tqsdk::TradingCalendar::Phase::PreOpen);
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 5, 9, 10)), tqsdk::TradingCalendar::Phase::Open);
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 5, 10, 20)),
              tqsdk::TradingCalendar::Phase::Break);
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 5, 11, 0)), tqsdk::TradingCalendar::Phase::Open);
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 5, 12, 0)), tqsdk::TradingCalendar::Phase::Break);
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 5, 14, 0)), tqsdk::TradingCalendar::Phase::Open);
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 5, 15, 30)),
              tqsdk::TradingCalendar::Phase::Settlement);
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 5, 20, 50)),
              tqsdk::TradingCalendar::Phase::PreOpen);
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 5, 21, 30)), tqsdk::TradingCalendar::Phase::Open);
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 5, 23, 30)), tqsdk::TradingCalendar::Phase::Open);
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 5, 2, 45)),
              tqsdk::TradingCalendar::Phase::Closed);
}

TEST(TradingCalendar, WeekendIsClosed) {
    tqsdk::TradingCalendar calendar;
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 7, 10, 0)),
              tqsdk::TradingCalendar::Phase::Closed);
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 8, 21, 30)),
              tqsdk::TradingCalendar::Phase::Closed);
}

TEST(TradingCalendar, NamesAndExpectedDisconnect) {
    using tqsdk::TradingCalendar;
    EXPECT_EQ(TradingCalendar::name(TradingCalendar::Phase::Open), "open");
    EXPECT_EQ(TradingCalendar::name(TradingCalendar::Phase::Settlement), "settlement");
    EXPECT_TRUE(TradingCalendar::is_expected_disconnect(TradingCalendar::Phase::Settlement));
    EXPECT_TRUE(TradingCalendar::is_expected_disconnect(TradingCalendar::Phase::Closed));
    EXPECT_FALSE(TradingCalendar::is_expected_disconnect(TradingCalendar::Phase::Open));
    EXPECT_FALSE(TradingCalendar::is_expected_disconnect(TradingCalendar::Phase::Reconnecting));
}

TEST(TradingCalendar, CustomSessions) {
    tqsdk::TradingCalendar::Config cfg;
    cfg.day_sessions            = {{10 * 60, 11 * 60}};
    cfg.night_sessions          = {};
    cfg.pre_open_minutes        = 5;
    cfg.settlement_start_minute = 11 * 60;
    cfg.settlement_end_minute   = 12 * 60;
    tqsdk::TradingCalendar calendar(cfg);
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 6, 9, 55)),
              tqsdk::TradingCalendar::Phase::PreOpen);
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 6, 10, 30)), tqsdk::TradingCalendar::Phase::Open);
    EXPECT_EQ(calendar.phase_at(make_tm(2026, 11, 6, 11, 30)),
              tqsdk::TradingCalendar::Phase::Settlement);
}
