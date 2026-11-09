#include "tqsdk/pch.h"

#include "tqsdk/trading/order_normalizer.h"
#include "tqsdk/trading/risk_manager.h"

#include <gtest/gtest.h>

namespace {

using namespace tqsdk;

InsertOrderReq open_order(std::string symbol = "SHFE.rb2701", uint64_t volume = 1) {
    InsertOrderReq req;
    req.instrument_id = std::move(symbol);
    req.direction     = Direction::BUY;
    req.offset        = Offset::OPEN;
    req.price_type    = PriceType::LIMIT;
    req.limit_price   = 3000;
    req.volume        = volume;
    return req;
}

TEST(RiskRule, OpenCountsLimitRejectsAfterLimit) {
    OpenCountsLimitRule rule(2, {"SHFE.rb2701"});
    const auto          req = open_order();
    EXPECT_FALSE(rule.check_insert(req).has_value());
    rule.on_insert(req);
    EXPECT_FALSE(rule.check_insert(req).has_value());
    rule.on_insert(req);
    EXPECT_TRUE(rule.check_insert(req).has_value());
    rule.on_new_trading_day();
    EXPECT_FALSE(rule.check_insert(req).has_value());
}

TEST(RiskRule, OpenVolumesLimitRejectsBeforeCrossingLimit) {
    OpenVolumesLimitRule rule(5, {"SHFE.rb2701"});
    EXPECT_FALSE(rule.check_insert(open_order("SHFE.rb2701", 3)).has_value());
    rule.on_insert(open_order("SHFE.rb2701", 3));
    EXPECT_TRUE(rule.check_insert(open_order("SHFE.rb2701", 3)).has_value());
    EXPECT_FALSE(rule.check_insert(open_order("SHFE.rb2701", 2)).has_value());
}

TEST(RiskRule, OrderRateLimitPrunesOldWindow) {
    OrderRateLimitRule rule(1, std::chrono::milliseconds(20));
    const auto         req = open_order();
    EXPECT_FALSE(rule.check_insert(req).has_value());
    rule.on_insert(req);
    EXPECT_TRUE(rule.check_insert(req).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(rule.check_insert(req).has_value());
}

TEST(RiskManager, RunsRulesAndSupportsRemoval) {
    RiskManager manager;
    auto rule = std::make_shared<OpenCountsLimitRule>(1, std::vector<std::string>{"SHFE.rb2701"});
    manager.add(rule);
    EXPECT_EQ(manager.size(), 1u);
    EXPECT_FALSE(manager.check_insert(open_order()).has_value());
    manager.on_insert(open_order());
    EXPECT_TRUE(manager.check_insert(open_order()).has_value());
    manager.remove(rule);
    EXPECT_FALSE(manager.check_insert(open_order()).has_value());
}

TEST(AdvancedOrder, MapsFakAndFokToCtpConditions) {
    auto fak                 = open_order();
    fak.advanced             = AdvancedOrder::FAK;
    const auto fak_effective = apply_advanced_order(fak);
    EXPECT_EQ(fak_effective.time_cond, TimeCondition::IOC);
    EXPECT_EQ(fak_effective.volume_cond, VolumeCondition::ANY);

    auto fok                 = open_order();
    fok.advanced             = AdvancedOrder::FOK;
    const auto fok_effective = apply_advanced_order(fok);
    EXPECT_EQ(fok_effective.time_cond, TimeCondition::IOC);
    EXPECT_EQ(fok_effective.volume_cond, VolumeCondition::ALL);

    auto five                 = open_order();
    five.price_type           = PriceType::FIVELEVEL;
    five.advanced             = AdvancedOrder::FAK;
    const auto five_effective = apply_advanced_order(five);
    EXPECT_EQ(five_effective.price_type, PriceType::FIVELEVEL);
    EXPECT_EQ(five_effective.time_cond, TimeCondition::IOC);
}

TEST(AdvancedOrder, JsonRoundtripsOfficialAdvancedFields) {
    const auto json = nlohmann::json{{"instrument_id", "CFFEX.IF2611"},
                                     {"direction", "BUY"},
                                     {"offset", "OPEN"},
                                     {"volume", 1},
                                     {"price_type", "FIVELEVEL"},
                                     {"advanced", "FAK"},
                                     {"offset_leg2", "CLOSETODAY"}};
    const auto req  = json.get<InsertOrderReq>();
    EXPECT_EQ(req.price_type, PriceType::FIVELEVEL);
    EXPECT_EQ(req.advanced, AdvancedOrder::FAK);
    EXPECT_EQ(req.offset_leg2, "CLOSETODAY");

    auto null_advanced = nlohmann::json{{"instrument_id", "SHFE.rb2701"},
                                        {"volume", 1},
                                        {"advanced", nullptr},
                                        {"offset_leg2", nullptr}};
    const auto parsed = null_advanced.get<InsertOrderReq>();
    EXPECT_EQ(parsed.advanced, AdvancedOrder::NONE);
    EXPECT_TRUE(parsed.offset_leg2.empty());
}

} // namespace
