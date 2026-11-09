#include "tqsdk/pch.h"

#include "tqsdk/backtest/backtest_engine.h"

#include <gtest/gtest.h>

namespace {

using namespace tqsdk;

BacktestBar bar(TimeStamp t, double close) {
    return BacktestBar{t, close, close + 1, close - 1, close, 100};
}

TEST(BacktestEngine, LongRoundTripRealizesPnl) {
    BacktestConfig config;
    config.volume_multiple = 10.0;
    config.commission_per_lot = 0.0;
    BacktestEngine engine(config);

    const std::vector<BacktestBar> bars{
        bar(1, 100), bar(2, 110), bar(3, 120)};

    const auto stats = engine.run(bars, [](BacktestContext& ctx) {
        if (ctx.bar().datetime == 1) {
            ctx.buy_open(2);
        } else if (ctx.bar().datetime == 2) {
            ctx.sell_close(2);
        }
    });

    EXPECT_EQ(stats.bars, 3u);
    EXPECT_EQ(stats.fills, 2u);
    EXPECT_EQ(stats.final_position, 0);
    EXPECT_DOUBLE_EQ(stats.realized_pnl, 200.0);
    EXPECT_DOUBLE_EQ(stats.final_equity, config.initial_balance + 200.0);
    EXPECT_EQ(stats.win_fills, 1u);
}

TEST(BacktestEngine, TargetPositionHandlesShortAndCommission) {
    BacktestConfig config;
    config.volume_multiple = 10.0;
    config.commission_per_lot = 1.0;
    BacktestEngine engine(config);

    const std::vector<BacktestBar> bars{bar(1, 100), bar(2, 90)};
    const auto stats = engine.run(bars, [](BacktestContext& ctx) {
        if (ctx.bar().datetime == 1) {
            ctx.target_position(-1);
        } else if (ctx.bar().datetime == 2) {
            ctx.target_position(0);
        }
    });

    EXPECT_EQ(stats.final_position, 0);
    EXPECT_DOUBLE_EQ(stats.realized_pnl, 100.0);
    EXPECT_DOUBLE_EQ(stats.commission, 2.0);
    EXPECT_DOUBLE_EQ(stats.final_equity, config.initial_balance + 98.0);
    EXPECT_EQ(stats.fills, 2u);
}

TEST(BacktestEngine, TracksOpenPositionMarkToMarket) {
    BacktestConfig config;
    config.volume_multiple = 2.0;
    BacktestEngine engine(config);

    const std::vector<BacktestBar> bars{bar(1, 100), bar(2, 105)};
    const auto stats = engine.run(bars, [](BacktestContext& ctx) {
        if (ctx.bar().datetime == 1) ctx.buy_open(1);
    });

    EXPECT_EQ(stats.final_position, 1);
    EXPECT_DOUBLE_EQ(stats.final_equity, config.initial_balance + 10.0);
    EXPECT_GE(stats.max_drawdown, 0.0);
}

} // namespace
