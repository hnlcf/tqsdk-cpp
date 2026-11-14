#include "tqsdk/pch.h"

#include "tqsdk/gateway/tq/tq_protocol.h"

#include <gtest/gtest.h>

using namespace tqsdk;
using namespace tqsdk::tq;

TEST(TqProtocol, AidNamesAreStable) {
    static_assert(aid_name(Aid::ReqLogin) == "req_login");
    static_assert(aid_name(Aid::RtnData) == "rtn_data");
    EXPECT_EQ(aid_name(Aid::ConfirmSettlement), "confirm_settlement");
    EXPECT_EQ(aid_name(Aid::SetRiskManagementRule), "set_risk_management_rule");
    EXPECT_EQ(parse_aid("pre_insert_order"), Aid::PreInsertOrder);
    EXPECT_EQ(parse_aid("bogus"), Aid::Unknown);
}

TEST(TqProtocol, LoginRequestRoundtrip) {
    LoginRequest login;
    login.bid                  = "simnow";
    login.user_name            = "user";
    login.password             = "pass";
    login.client_mac_address   = "AA-BB-CC";
    login.client_app_id        = "SHINNY_TQ_1.0";
    login.client_system_info   = "sys";
    login.front                = "tcp://127.0.0.1:1";
    login.backend              = "ctp";
    login.trading_fronts       = nlohmann::json::array({"td", "mc"});

    const auto serialized = serialize(Message{login});
    EXPECT_EQ(serialized["aid"], "req_login");
    EXPECT_EQ(serialized["password"], "pass");
    ASSERT_TRUE(serialized.contains("trading_fronts"));

    const auto parsed = parse(serialized);
    ASSERT_TRUE(parsed);
    const auto& out = std::get<LoginRequest>(*parsed);
    EXPECT_EQ(out.bid, "simnow");
    EXPECT_EQ(out.user_name, "user");
    EXPECT_EQ(out.backend, "ctp");
    EXPECT_EQ(out.trading_fronts->at(1), "mc");
}

TEST(TqProtocol, ClientRequestsRoundtrip) {
    const auto quote = parse(serialize(Message{SubscribeQuote{"DCE.v2701,DCE.i2701"}}));
    ASSERT_TRUE(quote);
    EXPECT_EQ(std::get<SubscribeQuote>(*quote).ins_list, "DCE.v2701,DCE.i2701");

    SetChart chart;
    chart.chart_id      = "chart-1";
    chart.ins_list      = "DCE.v2701";
    chart.duration      = 60'000'000'000;
    chart.view_width    = 500;
    chart.focus_datetime = 123;
    const auto chart_roundtrip = parse(serialize(Message{chart}));
    ASSERT_TRUE(chart_roundtrip);
    EXPECT_EQ(std::get<SetChart>(*chart_roundtrip).focus_datetime, 123);

    SetChartData data;
    data.symbol   = "DCE.v2701";
    data.dur_nano = 60'000'000'000;
    data.datas    = nlohmann::json{{"line-1", {{"type", "LINE"}}}};
    const auto data_roundtrip = parse(serialize(Message{data}));
    ASSERT_TRUE(data_roundtrip);
    EXPECT_EQ(std::get<SetChartData>(*data_roundtrip).datas["line-1"]["type"], "LINE");

    const auto trading = parse(
        serialize(Message{SubscribeTradingStatus{"SHFE.rb2701,CZCE.SA701"}}));
    ASSERT_TRUE(trading);
    EXPECT_EQ(std::get<SubscribeTradingStatus>(*trading).ins_list, "SHFE.rb2701,CZCE.SA701");
}

TEST(TqProtocol, OrderMessagesRoundtrip) {
    InsertOrder order;
    order.account_key      = "A";
    order.user_id          = "user";
    order.order_id         = "rid-1";
    order.exchange_id      = "DCE";
    order.instrument_id    = "v2701";
    order.direction        = Direction::SELL;
    order.offset           = Offset::CLOSE;
    order.volume           = 1;
    order.price_type       = PriceType::LIMIT;
    order.limit_price      = 5000.0;
    order.time_condition   = "GFD";
    order.volume_condition = "ANY";

    const auto serialized = serialize(Message{order});
    EXPECT_EQ(serialized["aid"], "insert_order");
    EXPECT_EQ(serialized["direction"], "SELL");
    EXPECT_EQ(serialized["limit_price"], 5000.0);

    const auto parsed = parse(serialized);
    ASSERT_TRUE(parsed);
    const auto& out = std::get<InsertOrder>(*parsed);
    EXPECT_EQ(out.exchange_id, "DCE");
    EXPECT_EQ(out.instrument_id, "v2701");
    EXPECT_EQ(out.direction, Direction::SELL);
    EXPECT_EQ(out.offset, Offset::CLOSE);
    ASSERT_TRUE(out.limit_price.has_value());
    EXPECT_DOUBLE_EQ(*out.limit_price, 5000.0);

    CancelOrder cancel;
    cancel.account_key = "A";
    cancel.user_id     = "user";
    cancel.order_id    = "rid-1";
    const auto cancel_roundtrip = parse(serialize(Message{cancel}));
    ASSERT_TRUE(cancel_roundtrip);
    EXPECT_EQ(std::get<CancelOrder>(*cancel_roundtrip).order_id, "rid-1");

    PreInsertOrder pre;
    pre.account_key   = "A";
    pre.user_id       = "user";
    pre.exchange_id   = "DCE";
    pre.instrument_id = "v2701";
    const auto pre_roundtrip = parse(serialize(Message{pre}));
    ASSERT_TRUE(pre_roundtrip);
    EXPECT_EQ(std::get<PreInsertOrder>(*pre_roundtrip).volume, 1u);
    EXPECT_EQ(std::get<PreInsertOrder>(*pre_roundtrip).hedge_flag, "SPECULATION");
}

TEST(TqProtocol, ServerMessagesRoundtrip) {
    RtnData rtn;
    rtn.data = nlohmann::json::array({
        nlohmann::json{{"quotes", {{"DCE.v2701", {{"last_price", 5000.0}}}}}},
        nlohmann::json{{"trade", {{"user", {{"trade_more_data", false}}}}}},
    });
    const auto rtn_roundtrip = parse(serialize(Message{rtn}));
    ASSERT_TRUE(rtn_roundtrip);
    EXPECT_EQ(std::get<RtnData>(*rtn_roundtrip).data.size(), 2u);

    RtnBrokers brokers;
    brokers.capabilities = nlohmann::json{{"shfe_ine_combination", true}};
    const auto brokers_roundtrip = parse(serialize(Message{brokers}));
    ASSERT_TRUE(brokers_roundtrip);
    EXPECT_TRUE(std::get<RtnBrokers>(*brokers_roundtrip).capabilities["shfe_ine_combination"]);

    const auto login = parse(nlohmann::json{{"aid", "rsp_login"}, {"code", 0}});
    ASSERT_TRUE(login);
    EXPECT_EQ(std::get<RspLogin>(*login).raw["code"], 0);

    const auto quotes = parse(nlohmann::json{{"aid", "rsp_quotes"}, {"quotes", "ok"}});
    ASSERT_TRUE(quotes);
    EXPECT_EQ(std::get<RspQuotes>(*quotes).raw["quotes"], "ok");
}

TEST(TqProtocol, UnknownAidReturnsError) {
    const auto result = parse(nlohmann::json{{"aid", "not_a_tq_message"}});
    EXPECT_FALSE(result);
    EXPECT_NE(result.error().detail.find("unknown"), std::string::npos);
}

TEST(TqProtocol, ControlMessagesSerialize) {
    EXPECT_EQ(serialize(Message{ConfirmSettlement{}})["aid"], "confirm_settlement");
    EXPECT_EQ(serialize(Message{PeekMessage{}})["aid"], "peek_message");
    EXPECT_EQ(serialize(Message{Heartbeat{}})["aid"], "heartbeat");
    EXPECT_EQ(serialize(Message{Terminate{}})["aid"], "terminate");
    EXPECT_EQ(serialize(Message{Ratio{2.0}})["speed"], 2.0);
}
