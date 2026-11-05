#include "tqsdk/pch.h"

#include "tqsdk/serve/ws_protocol.h"

using namespace tqsdk::serve;

namespace {

const WsSubscribe& as_subscribe(const WsClientMessage& msg) {
    return std::get<WsSubscribe>(msg);
}

} // namespace

TEST(WsProtocol, ParseSubscribeMessage) {
    auto msg = parse_ws_client_message(R"({
        "op":"subscribe",
        "symbols":["SHFE.rb2610","DCE.i2601"],
        "klines":[{"symbol":"SHFE.rb2610","duration_ns":60000000000,"view_width":50}],
        "topics":["account","orders"]
    })");

    ASSERT_TRUE(std::holds_alternative<WsSubscribe>(msg));
    const auto& sub = as_subscribe(msg);
    ASSERT_EQ(sub.symbols.size(), 2u);
    EXPECT_EQ(sub.symbols[0], "SHFE.rb2610");
    ASSERT_EQ(sub.klines.size(), 1u);
    EXPECT_EQ(sub.klines[0].duration_ns, 60'000'000'000LL);
    EXPECT_EQ(sub.klines[0].view_width, 50);
    ASSERT_EQ(sub.topics.size(), 2u);
    EXPECT_EQ(sub.topics[1], "orders");
}

TEST(WsProtocol, ParseUnsubscribeFromStdString) {
    const std::string text = R"({"op":"unsubscribe","symbols":["SHFE.rb2610"],"klines":[]})";
    auto              msg  = parse_ws_client_message(text);
    ASSERT_TRUE(std::holds_alternative<WsUnsubscribe>(msg));
    EXPECT_EQ(std::get<WsUnsubscribe>(msg).symbols[0], "SHFE.rb2610");
}

TEST(WsProtocol, ParsePing) {
    auto msg = parse_ws_client_message(R"({"op":"ping"})");
    EXPECT_TRUE(std::holds_alternative<WsPing>(msg));
}

TEST(WsProtocol, InvalidKlineEntriesAreDropped) {
    auto msg = parse_ws_client_message(R"({
        "op":"subscribe",
        "symbols":["", "SHFE.rb2610"],
        "klines":[
            {"symbol":"","duration_ns":100},
            {"symbol":"SHFE.rb2610","duration_ns":0},
            {"symbol":"SHFE.rb2610","duration_ns":100,"view_width":0}
        ]
    })");

    const auto& sub = as_subscribe(msg);
    ASSERT_EQ(sub.symbols.size(), 1u);
    EXPECT_EQ(sub.symbols[0], "SHFE.rb2610");
    ASSERT_EQ(sub.klines.size(), 1u);
    EXPECT_EQ(sub.klines[0].view_width, 200) << "non-positive view_width should default";
}

TEST(WsProtocol, RejectBadClientMessages) {
    EXPECT_THROW(parse_ws_client_message("not json"), WsProtocolError);
    EXPECT_THROW(parse_ws_client_message(R"({"symbols":[]})"), WsProtocolError);
    EXPECT_THROW(parse_ws_client_message(R"({"op":"bogus"})"), WsProtocolError);
    EXPECT_THROW(parse_ws_client_message("[]"), WsProtocolError);
    EXPECT_THROW(parse_ws_client_message(R"({"op":"subscribe","symbols":123})"), WsProtocolError);
}

TEST(WsProtocol, ClientMessageRoundtrip) {
    WsSubscribe sub;
    sub.symbols = {"SHFE.rb2610"};
    sub.klines  = {{"SHFE.rb2610", 60'000'000'000LL, 100}};
    sub.topics  = {"account"};

    const std::string text = serialize_ws_message(WsClientMessage{std::move(sub)});
    auto              msg  = parse_ws_client_message(text);

    ASSERT_TRUE(std::holds_alternative<WsSubscribe>(msg));
    const auto& out = std::get<WsSubscribe>(msg);
    EXPECT_EQ(out.symbols, std::vector<std::string>{"SHFE.rb2610"});
    ASSERT_EQ(out.klines.size(), 1u);
    EXPECT_EQ(out.klines[0].duration_ns, 60'000'000'000LL);
    EXPECT_EQ(out.topics, std::vector<std::string>{"account"});
}

TEST(WsProtocol, SerializeWelcome) {
    WsWelcome welcome;
    welcome.protocol              = kWsProtocolVersion;
    welcome.heartbeat_interval_ms = 1500;
    const Json j                  = Json::parse(serialize_ws_message(welcome));

    EXPECT_EQ(j["type"], "welcome");
    EXPECT_EQ(j["protocol"], kWsProtocolVersion);
    EXPECT_EQ(j["heartbeat_interval_ms"], 1500);
}

TEST(WsProtocol, ServerDiffRoundtrip) {
    WsDiff diff;
    diff.seq          = 42;
    diff.timestamp_ns = 123456789;
    diff.data         = Json{{"quotes", {{"SHFE.rb2610", {{"last_price", 3800.0}}}}}};

    const std::string text = serialize_ws_message(WsServerMessage{std::move(diff)});
    auto              msg  = parse_ws_server_message(text);

    ASSERT_TRUE(std::holds_alternative<WsDiff>(msg));
    const auto& out = std::get<WsDiff>(msg);
    EXPECT_EQ(out.seq, 42u);
    EXPECT_EQ(out.timestamp_ns, 123456789);
    EXPECT_DOUBLE_EQ(out.data["quotes"]["SHFE.rb2610"]["last_price"].get<double>(), 3800.0);
}

TEST(WsProtocol, ServerErrorAndPongRoundtrip) {
    auto err_msg = parse_ws_server_message(serialize_ws_message(WsServerMessage{WsError{"boom"}}));
    ASSERT_TRUE(std::holds_alternative<WsError>(err_msg));
    EXPECT_EQ(std::get<WsError>(err_msg).error, "boom");

    auto pong = parse_ws_server_message(serialize_ws_message(WsServerMessage{WsPong{}}));
    EXPECT_TRUE(std::holds_alternative<WsPong>(pong));
}

TEST(WsProtocol, RejectBadServerMessages) {
    EXPECT_THROW(parse_ws_server_message("not json"), WsProtocolError);
    EXPECT_THROW(parse_ws_server_message(R"({"type":"bogus"})"), WsProtocolError);
    EXPECT_THROW(parse_ws_server_message(R"({})"), WsProtocolError);
    EXPECT_THROW(parse_ws_server_message(R"({"type":"diff","seq":"x"})"), WsProtocolError);
}

TEST(WsProtocol, NumericDiscriminatorIsProtocolError) {
    EXPECT_THROW(parse_ws_client_message(R"({"op":1})"), WsProtocolError);
    EXPECT_THROW(parse_ws_client_message(R"({"op":true})"), WsProtocolError);
    EXPECT_THROW(parse_ws_server_message(R"({"type":1})"), WsProtocolError);
    EXPECT_THROW(parse_ws_server_message(R"({"type":true})"), WsProtocolError);
}

TEST(WsProtocol, TopicsPresenceIsDistinctFromEmptyTopics) {
    const auto no_topics =
        parse_ws_client_message(R"({"op":"subscribe","symbols":["SHFE.rb2610"],"klines":[]})");
    ASSERT_TRUE(std::holds_alternative<WsSubscribe>(no_topics));
    EXPECT_FALSE(as_subscribe(no_topics).topics_provided);

    const auto empty_topics = parse_ws_client_message(
        R"({"op":"subscribe","symbols":["SHFE.rb2610"],"klines":[],"topics":[]})");
    ASSERT_TRUE(std::holds_alternative<WsSubscribe>(empty_topics));
    EXPECT_TRUE(as_subscribe(empty_topics).topics_provided);
}
