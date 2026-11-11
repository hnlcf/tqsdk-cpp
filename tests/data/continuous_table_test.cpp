#include "tqsdk/pch.h"

#include "tqsdk/data/continuous_table.h"

#include <gtest/gtest.h>

using namespace tqsdk;

namespace {

std::int64_t day_ns(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon  = month - 1;
    tm.tm_mday = day;
    return static_cast<std::int64_t>(timegm(&tm)) * 1'000'000'000LL;
}

KlineBar bar(int year, int month, int day, double close) {
    KlineBar out;
    out.datetime = day_ns(year, month, day);
    out.open = out.high = out.low = close;
    out.close                     = close;
    return out;
}

} // namespace

TEST(ContinuousTable, ParsesAndSelectsRolls) {
    ContinuousTableConfig cfg;
    cfg.enabled = false;
    ContinuousTable table(cfg);
    ASSERT_TRUE(table.parse(R"({
      "DCE.jm": [["20240101", "DCE.jm2405"], ["20240201", "DCE.jm2409"]]
    })"));
    ASSERT_TRUE(table.ready());
    EXPECT_EQ(table.product_of("KQ.m@DCE.jm"), "DCE.jm");
    EXPECT_TRUE(table.is_main_continuous("KQ.m@DCE.jm"));
    EXPECT_FALSE(table.is_main_continuous("KQ.i@DCE.jm"));
    const auto rolls = table.rolls_for("DCE.jm", "20240101", "20241231");
    ASSERT_EQ(rolls.size(), 1u);
    EXPECT_EQ(rolls[0].date, "20240201");
    EXPECT_EQ(rolls[0].from, "DCE.jm2405");
    EXPECT_EQ(rolls[0].to, "DCE.jm2409");
}

TEST(ContinuousTable, ForwardAndBackAdjustRollover) {
    const std::vector<KlineBar> raw{
        bar(2024, 1, 2, 100), bar(2024, 1, 3, 101), bar(2024, 2, 2, 200), bar(2024, 2, 5, 202)};
    const std::vector<ContinuousRoll>            rolls{{"20240201", "DCE.jm2405", "DCE.jm2409"}};
    std::map<std::string, std::vector<KlineBar>> contracts{
        {"DCE.jm2405", {bar(2024, 1, 3, 100)}},
        {"DCE.jm2409", {bar(2024, 2, 2, 200)}},
    };

    const auto forward =
        ContinuousTable::adjust_daily(raw, rolls, contracts, KlineAdjustMode::Forward);
    ASSERT_TRUE(forward.adjusted);
    ASSERT_EQ(forward.bars.size(), raw.size());
    EXPECT_DOUBLE_EQ(forward.bars[0].close, 200.0);
    EXPECT_DOUBLE_EQ(forward.bars[1].close, 202.0);
    EXPECT_DOUBLE_EQ(forward.bars[2].close, 200.0);
    EXPECT_DOUBLE_EQ(forward.bars[3].close, 202.0);

    const auto back = ContinuousTable::adjust_daily(raw, rolls, contracts, KlineAdjustMode::Back);
    ASSERT_TRUE(back.adjusted);
    EXPECT_DOUBLE_EQ(back.bars[0].close, 100.0);
    EXPECT_DOUBLE_EQ(back.bars[1].close, 101.0);
    EXPECT_DOUBLE_EQ(back.bars[2].close, 100.0);
    EXPECT_DOUBLE_EQ(back.bars[3].close, 101.0);
}

TEST(ContinuousTable, MissingContractBarsIsReported) {
    const std::vector<KlineBar>       raw{bar(2024, 1, 2, 100), bar(2024, 1, 3, 101)};
    const std::vector<ContinuousRoll> rolls{{"20240102", "A", "B"}};
    const auto result = ContinuousTable::adjust_daily(raw, rolls, {}, KlineAdjustMode::Forward);
    EXPECT_FALSE(result.adjusted);
    EXPECT_FALSE(result.warnings.empty());
}
