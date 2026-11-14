#include "tqsdk/pch.h"

#include "tqsdk/gateway/tq/tq_td_gateway.h"

#include <gtest/gtest.h>

using namespace tqsdk;

TEST(TqTdNormalization, WrapsOtgTradeNodeInUserLayer) {
    nlohmann::json slice    = nlohmann::json::object();
    nlohmann::json node     = nlohmann::json::object();
    node["accounts"]        = nlohmann::json{{"CNY", {{"balance", 1000.0}}}};
    node["trade_more_data"] = false;
    slice["trade"]          = nlohmann::json{{"simnow_user", node}};

    const auto normalized = normalize_td_trade_slice(slice, "A");
    ASSERT_TRUE(normalized.contains("trade"));
    ASSERT_TRUE(normalized["trade"].contains("A"));
    const auto& out = normalized["trade"]["A"];
    ASSERT_TRUE(out.contains("user"));
    EXPECT_DOUBLE_EQ(out["user"]["accounts"]["CNY"]["balance"].get<double>(), 1000.0);
    EXPECT_TRUE(td_trade_complete(normalized));
}

TEST(TqTdNormalization, KeepsAlreadyWrappedNodes) {
    nlohmann::json node     = nlohmann::json::object();
    node["user"]            = nlohmann::json{{"positions", {{"DCE.v2701", {{"volume_long", 1}}}}}};
    node["trade_more_data"] = true;
    nlohmann::json slice;
    slice["trade"] = nlohmann::json{{"simnow_user", node}};

    const auto  normalized = normalize_td_trade_slice(slice, "A");
    const auto& out        = normalized["trade"]["A"];
    ASSERT_TRUE(out.contains("user"));
    EXPECT_EQ(out["user"]["positions"]["DCE.v2701"]["volume_long"], 1);
    EXPECT_FALSE(td_trade_complete(normalized));
}

TEST(TqTdNormalization, IgnoresNonTradeSlices) {
    const nlohmann::json slice      = {{"quotes", {{"DCE.v2701", {{"last_price", 1.0}}}}}};
    const auto           normalized = normalize_td_trade_slice(slice, "A");
    EXPECT_EQ(normalized, slice);
    EXPECT_FALSE(td_trade_complete(normalized));
}

TEST(TqTdNormalization, GatewayStartsDisconnected) {
    asio::io_context     io;
    TqTdGateway::Options options;
    options.url         = "wss://127.0.0.1:1/trade";
    options.broker_name = "simnow";
    options.account_id  = "user";
    TqTdGateway gateway(io.get_executor(), std::move(options));
    EXPECT_FALSE(gateway.is_open());
    EXPECT_FALSE(gateway.snapshot_ready());
    EXPECT_FALSE(gateway.metrics().value("connected", true));
}
