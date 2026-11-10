#include "tqsdk/pch.h"

#include "tqsdk/option/option_analytics.h"

#include <gtest/gtest.h>

namespace {

using namespace tqsdk;

OptionSpec atm(OptionRight right) {
    OptionSpec spec;
    spec.spot           = 100.0;
    spec.strike         = 100.0;
    spec.time_to_expiry = 1.0;
    spec.risk_free_rate = 0.05;
    spec.dividend_yield = 0.0;
    spec.volatility     = 0.2;
    spec.right          = right;
    return spec;
}

TEST(OptionAnalytics, BlackScholesKnownCallValue) {
    auto spec           = atm(OptionRight::CALL);
    spec.risk_free_rate = 0.0;
    spec.dividend_yield = 0.0;
    const auto out      = black_scholes(spec);
    EXPECT_NEAR(out.price, 7.965567, 1e-5);
    EXPECT_GT(out.greeks.delta, 0.0);
    EXPECT_GT(out.greeks.gamma, 0.0);
    EXPECT_GT(out.greeks.vega, 0.0);
}

TEST(OptionAnalytics, PutCallParity) {
    auto         call_spec = atm(OptionRight::CALL);
    auto         put_spec  = atm(OptionRight::PUT);
    const auto   call      = black_scholes(call_spec);
    const auto   put       = black_scholes(put_spec);
    const double expected = call_spec.spot - call_spec.strike * std::exp(-call_spec.risk_free_rate *
                                                                         call_spec.time_to_expiry);
    EXPECT_NEAR(call.price - put.price, expected, 1e-9);
}

TEST(OptionAnalytics, ImpliedVolatilityRoundTrips) {
    auto       spec        = atm(OptionRight::CALL);
    const auto theoretical = black_scholes(spec).price;
    const auto iv          = implied_volatility(theoretical, spec);
    ASSERT_TRUE(iv.has_value());
    EXPECT_NEAR(*iv, spec.volatility, 1e-6);
}

TEST(OptionAnalytics, AtmStrikeChoosesNearest) {
    EXPECT_EQ(atm_strike(102.2, {90, 100, 105}), 100);
    EXPECT_EQ(atm_strike(104.9, {90, 100, 105}), 105);
    EXPECT_FALSE(atm_strike(100, {}).has_value());
}

TEST(OptionAnalytics, ChainContainsCallsAndPutsNearAtm) {
    const auto rows =
        compute_option_chain(100, {90, 95, 100, 105, 110}, 0.5, 0.03, 0.0, 0.2, {}, "TEST.OPT", 1);
    ASSERT_EQ(rows.size(), 6u); // 3 strikes * call/put
    EXPECT_EQ(rows[0].strike, 95);
    EXPECT_EQ(rows[0].right, OptionRight::CALL);
    EXPECT_EQ(rows[1].right, OptionRight::PUT);
    EXPECT_TRUE(rows[0].valuation.price > 0);
    EXPECT_FALSE(rows[0].has_market_price);
}

} // namespace
