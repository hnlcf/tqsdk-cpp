#include "tqsdk/pch.h"

#include "tqsdk/ipc/protocol.h"

#include "gtest/gtest.h"

namespace tqsdk {
namespace ipc {
namespace {

// ---------------- 各 body 的 JSON 往返 ----------------

template <typename Body>
Body roundtrip_json(const Body& in) {
    auto          bytes = encode_body(in); // 默认 JSON
    ReceivedFrame f{};
    f.type     = FrameType::Hello; // 类型不影响 decode_body<Body>
    f.encoding = FrameEncoding::Json;
    f.payload  = bytes;
    return decode_body<Body>(f);
}

TEST(ProtocolCodec, HelloRoundtrip) {
    HelloBody in;
    in.version = kIpcProtocolVersion;
    in.role    = IpcRole::Gateway;
    in.name    = "gw-1";
    auto out   = roundtrip_json(in);
    EXPECT_EQ(out.version, in.version);
    EXPECT_EQ(out.role, in.role);
    EXPECT_EQ(out.name, in.name);
}

TEST(ProtocolCodec, HelloAckRoundtrip) {
    HelloAckBody in{true, ""};
    auto         out = roundtrip_json(in);
    EXPECT_EQ(out.ok, true);
    EXPECT_EQ(out.error, "");

    HelloAckBody bad{false, "bad version"};
    auto         out2 = roundtrip_json(bad);
    EXPECT_EQ(out2.ok, false);
    EXPECT_EQ(out2.error, "bad version");
}

TEST(ProtocolCodec, SubscribeRoundtrip) {
    SubscribeBody in;
    in.symbols = {"SHFE.rb2610", "DCE.i2601"};
    in.klines  = {{"SHFE.rb2610", 60'000'000'000LL, 200}, {"DCE.i2601", 300'000'000'000LL, 50}};
    auto out   = roundtrip_json(in);
    ASSERT_EQ(out.symbols.size(), 2u);
    EXPECT_EQ(out.symbols, in.symbols);
    ASSERT_EQ(out.klines.size(), 2u);
    EXPECT_EQ(out.klines[0].symbol, "SHFE.rb2610");
    EXPECT_EQ(out.klines[0].duration_ns, 60'000'000'000LL);
    EXPECT_EQ(out.klines[0].view_width, 200);
    EXPECT_EQ(out.klines[1].symbol, "DCE.i2601");
    EXPECT_EQ(out.klines[1].view_width, 50);
}

TEST(ProtocolCodec, MarketDataPushRoundtrip) {
    MarketDataPushBody in;
    in.data  = {{"quotes", {{"SHFE.rb2610", {{"last_price", 3800.0}, {"volume", 100}}}}}};
    auto out = roundtrip_json(in);
    EXPECT_EQ(out.data, in.data);
}

TEST(ProtocolCodec, TradePushRoundtrip) {
    TradePushBody in;
    in.data  = {{"trade", {{"", {{"user", {{"orders", json::object()}}}}}}}};
    auto out = roundtrip_json(in);
    EXPECT_EQ(out.data, in.data);
}

TEST(ProtocolCodec, CommandInsertRoundtrip) {
    CommandBody in;
    in.op                   = CommandOp::InsertOrder;
    in.rid                  = "rid_7";
    in.insert.instrument_id = "SHFE.rb2610";
    in.insert.exchange_id   = "SHFE";
    in.insert.direction     = Direction::BUY;
    in.insert.offset        = Offset::OPEN;
    in.insert.price_type    = PriceType::LIMIT;
    in.insert.limit_price   = 3800.5;
    in.insert.time_cond     = TimeCondition::GFD;
    in.insert.volume_cond   = VolumeCondition::ANY;
    in.insert.volume        = 2;
    in.insert.min_volume    = 0;

    auto out = roundtrip_json(in);
    EXPECT_EQ(out.op, CommandOp::InsertOrder);
    EXPECT_EQ(out.rid, "rid_7");
    EXPECT_EQ(out.insert.instrument_id, "SHFE.rb2610");
    EXPECT_EQ(out.insert.direction, Direction::BUY);
    EXPECT_EQ(out.insert.offset, Offset::OPEN);
    EXPECT_EQ(out.insert.price_type, PriceType::LIMIT);
    EXPECT_DOUBLE_EQ(out.insert.limit_price, 3800.5);
    EXPECT_EQ(out.insert.volume, 2u);
}

TEST(ProtocolCodec, CommandCancelRoundtrip) {
    CommandBody in;
    in.op                       = CommandOp::CancelOrder;
    in.cancel.order_id          = "rid_9";
    in.cancel.exchange_order_id = "ex_123";

    auto out = roundtrip_json(in);
    EXPECT_EQ(out.op, CommandOp::CancelOrder);
    EXPECT_EQ(out.cancel.order_id, "rid_9");
    EXPECT_EQ(out.cancel.exchange_order_id, "ex_123");
}

TEST(ProtocolCodec, CommandAckRoundtrip) {
    CommandAckBody in;
    in.ack.success   = false;
    in.ack.order_id  = "rid_3";
    in.ack.error_msg = "rejected";

    auto out = roundtrip_json(in);
    EXPECT_EQ(out.ack.success, false);
    EXPECT_EQ(out.ack.order_id, "rid_3");
    EXPECT_EQ(out.ack.error_msg, "rejected");
}

TEST(ProtocolCodec, HeartbeatRoundtrip) {
    auto out = roundtrip_json(HeartbeatBody{});
    (void)out; // 空 body 只需不抛异常
}

TEST(ProtocolCodec, GoodbyeRoundtrip) {
    GoodbyeBody in{"shutdown"};
    auto        out = roundtrip_json(in);
    EXPECT_EQ(out.reason, "shutdown");
}

TEST(ProtocolCodec, OpaqueRoundtrip) {
    OpaqueBody in;
    in.data  = {{"anything", json::array({1, 2, 3})}};
    auto out = roundtrip_json(in);
    EXPECT_EQ(out.data, in.data);
}

// ---------------- MessagePack：验证序列化抽象可切换 ----------------

TEST(ProtocolCodec, MsgpackHelloEquivJson) {
    HelloBody in{1, IpcRole::Node, "node-x"};
    auto      mbytes = encode_body(in, FrameEncoding::MessagePack);
    auto      jbytes = encode_body(in, FrameEncoding::Json);

    ReceivedFrame fm{FrameType::Hello, FrameEncoding::MessagePack, mbytes};
    ReceivedFrame fj{FrameType::Hello, FrameEncoding::Json, jbytes};
    auto          out_m = decode_as<FrameType::Hello>(fm);
    auto          out_j = decode_as<FrameType::Hello>(fj);
    EXPECT_EQ(out_m.version, out_j.version);
    EXPECT_EQ(out_m.role, out_j.role);
    EXPECT_EQ(out_m.name, out_j.name);
    // msgpack 一般比 json 小
    EXPECT_LT(mbytes.size(), jbytes.size());
}

TEST(ProtocolCodec, MsgpackSubscribeEquivJson) {
    SubscribeBody in;
    in.symbols = {"SHFE.rb2610"};
    in.klines  = {{"DCE.i2601", 60'000'000'000LL, 200}};

    ReceivedFrame fm{FrameType::Subscribe,
                     FrameEncoding::MessagePack,
                     encode_body(in, FrameEncoding::MessagePack)};
    ReceivedFrame fj{
        FrameType::Subscribe, FrameEncoding::Json, encode_body(in, FrameEncoding::Json)};
    auto om = decode_as<FrameType::Subscribe>(fm);
    auto oj = decode_as<FrameType::Subscribe>(fj);
    EXPECT_EQ(om.symbols, oj.symbols);
    ASSERT_EQ(om.klines.size(), oj.klines.size());
    EXPECT_EQ(om.klines[0].symbol, oj.klines[0].symbol);
    EXPECT_EQ(om.klines[0].duration_ns, oj.klines[0].duration_ns);
}

// ---------------- decode_as 类型校验 ----------------

TEST(ProtocolCodec, DecodeAsTypeMismatchThrows) {
    auto          bytes = encode_body(HeartbeatBody{});
    ReceivedFrame f{FrameType::Heartbeat, FrameEncoding::Json, bytes};
    // 用 Goodbye 去解 Heartbeat 帧 → 应抛
    EXPECT_THROW(decode_as<FrameType::Goodbye>(f), IpcError);
}

TEST(ProtocolCodec, DecodeAsOk) {
    SubscribeAckBody in{true, ""};
    ReceivedFrame    f{FrameType::SubscribeAck, FrameEncoding::Json, encode_body(in)};
    auto             out = decode_as<FrameType::SubscribeAck>(f);
    EXPECT_EQ(out.ok, true);
}

// ---------------- 非法输入 ----------------

TEST(ProtocolCodec, DecodeMalformedJsonThrows) {
    std::vector<uint8_t> bad = {'{', 'n', 'o', 't', ' ', 'j'}; // 不完整 JSON
    ReceivedFrame        f{FrameType::Hello, FrameEncoding::Json, bad};
    EXPECT_THROW(decode_body<HelloBody>(f), IpcError);
}

TEST(ProtocolCodec, DecodeMissingFieldThrows) {
    // HelloBody 缺 version
    nlohmann::json dom = {{"role", "Gateway"}, {"name", "x"}};
    std::string    s   = dom.dump();
    ReceivedFrame  f{
        FrameType::Hello, FrameEncoding::Json, std::vector<uint8_t>(s.begin(), s.end())};
    EXPECT_THROW(decode_body<HelloBody>(f), IpcError);
}

TEST(ProtocolCodec, DecodeBytesMalformedMsgpackThrows) {
    std::vector<uint8_t> bad = {0xff, 0xff, 0xff}; // 非法 msgpack
    ReceivedFrame        f{FrameType::Hello, FrameEncoding::MessagePack, bad};
    EXPECT_THROW(decode_body<HelloBody>(f), IpcError);
}

// ---------------- encode_frame：Type 与 Body 不匹配应编译期报错（正向用例） ----------------

TEST(ProtocolCodec, EncodeFrameHelper) {
    SubscribeAckBody in{true, ""};
    auto [enc, payload] = encode_frame<FrameType::SubscribeAck>(in);
    EXPECT_EQ(enc, FrameEncoding::Json);
    ReceivedFrame f{FrameType::SubscribeAck, enc, payload};
    auto          out = decode_as<FrameType::SubscribeAck>(f);
    EXPECT_EQ(out.ok, true);
}

} // namespace
} // namespace ipc
} // namespace tqsdk
