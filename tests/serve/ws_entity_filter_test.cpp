#include "tqsdk/pch.h"

#include "tqsdk/serve/ws_entity_filter.h"

using namespace tqsdk::serve;

TEST(WsEntityFilter, DropsUnchangedOrdersAndTopics) {
    WsEntitySelection selection;
    selection.topics = {"orders"};
    WsEntityFilter filter;

    const nlohmann::json diff = {
        {"quotes", {{"SHFE.rb2701", {{"last_price", 3800.0}}}}},
        {"orders",
         {{"r1",
           {{"order_id", "r1"}, {"is_dead", false}, {"volume_left", 1}, {"last_msg", "未成交"}}}}},
    };

    auto first = filter.filter(selection, diff);
    EXPECT_TRUE(first.contains("orders"));
    EXPECT_FALSE(first.contains("quotes")); // no symbols selected
    EXPECT_EQ(first["orders"].size(), 1u);

    // CTP 全量查询会反复送来相同实体；第二个周期应被过滤。
    auto second = filter.filter(selection, diff);
    EXPECT_TRUE(second.empty());

    auto changed                           = diff;
    changed["orders"]["r1"]["volume_left"] = 0;
    auto third                             = filter.filter(selection, changed);
    ASSERT_TRUE(third.contains("orders"));
    EXPECT_EQ(third["orders"]["r1"]["volume_left"], 0);
}

TEST(WsEntityFilter, NoTopicsDropsTradingEntities) {
    WsEntitySelection selection;
    selection.symbols = {"SHFE.rb2701"};
    WsEntityFilter       filter;
    const nlohmann::json diff = {
        {"quotes", {{"SHFE.rb2701", {{"last_price", 3800.0}}}}},
        {"account", {{"balance", 1000.0}}},
        {"orders", {{"r1", {{"order_id", "r1"}}}}},
    };
    auto out = filter.filter(selection, diff);
    EXPECT_TRUE(out.contains("quotes"));
    EXPECT_FALSE(out.contains("account"));
    EXPECT_FALSE(out.contains("orders"));
}
