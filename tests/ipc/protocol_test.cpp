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
    in.release = true;
    auto out   = roundtrip_json(in);
    ASSERT_EQ(out.symbols.size(), 2u);
    EXPECT_EQ(out.symbols, in.symbols);
    EXPECT_TRUE(out.release);
    ASSERT_EQ(out.klines.size(), 2u);
    EXPECT_EQ(out.klines[0].symbol, "SHFE.rb2610");
    EXPECT_EQ(out.klines[0].duration_ns, 60'000'000'000LL);
    EXPECT_EQ(out.klines[0].view_width, 200);
    EXPECT_EQ(out.klines[1].symbol, "DCE.i2601");
    EXPECT_EQ(out.klines[1].view_width, 50);
}

TEST(ProtocolCodec, MarketDataPushRoundtrip) {
    MarketDataPushBody in;
    in.meta  = StreamMeta{"epoch-x", 17};
    in.data  = {{"quotes", {{"SHFE.rb2610", {{"last_price", 3800.0}, {"volume", 100}}}}}};
    auto out = roundtrip_json(in);
    EXPECT_EQ(out.data, in.data);
    EXPECT_EQ(out.meta.epoch, "epoch-x");
    EXPECT_EQ(out.meta.seq, 17u);
}

TEST(ProtocolCodec, TradePushRoundtrip) {
    TradePushBody in;
    in.meta  = StreamMeta{"epoch-y", 3};
    in.data  = {{"trade", {{"", {{"user", {{"orders", json::object()}}}}}}}};
    auto out = roundtrip_json(in);
    EXPECT_EQ(out.data, in.data);
    EXPECT_EQ(out.meta.epoch, "epoch-y");
    EXPECT_EQ(out.meta.seq, 3u);
}

TEST(ProtocolCodec, ResumeRoundtrip) {
    ResumeBody in;
    in.epoch       = "epoch-z";
    in.md_next_seq = 101;
    in.td_next_seq = 9;
    auto out       = roundtrip_json(in);
    EXPECT_EQ(out.epoch, in.epoch);
    EXPECT_EQ(out.md_next_seq, 101u);
    EXPECT_EQ(out.td_next_seq, 9u);
}

TEST(ProtocolCodec, ResumeAckRoundtrip) {
    ResumeAckBody in;
    in.epoch                = "epoch-z";
    in.md_replay            = true;
    in.td_replay            = false;
    in.md_from_seq          = 101;
    in.td_from_seq          = 9;
    in.td_snapshot_required = true;
    in.error                = "";
    auto out                = roundtrip_json(in);
    EXPECT_EQ(out.epoch, in.epoch);
    EXPECT_TRUE(out.md_replay);
    EXPECT_FALSE(out.td_replay);
    EXPECT_EQ(out.md_from_seq, 101u);
    EXPECT_EQ(out.td_from_seq, 9u);
    EXPECT_TRUE(out.td_snapshot_required);
}

TEST(ProtocolCodec, TradeSnapshotRoundtrip) {
    TradeSnapshotBody in;
    in.meta  = StreamMeta{"epoch-z", 10};
    in.data  = {{"trade", {{"", {{"user", {{"orders", json::object()}}}}}}}};
    in.ready = false;
    auto out = roundtrip_json(in);
    EXPECT_EQ(out.meta.epoch, in.meta.epoch);
    EXPECT_EQ(out.meta.seq, in.meta.seq);
    EXPECT_EQ(out.data, in.data);
    EXPECT_FALSE(out.ready);

    // 旧 gateway 不发送 ready 字段时默认视为最新快照。
    ReceivedFrame legacy_frame{};
    legacy_frame.encoding = FrameEncoding::Json;
    legacy_frame.payload  = encode_body(json{{"meta", in.meta}, {"data", in.data}});
    auto legacy           = decode_body<TradeSnapshotBody>(legacy_frame);
    EXPECT_TRUE(legacy.ready);
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

// 市价单/未知价：limit_price = NaN(kUnknownDouble) 经 JSON 往返必须保持 NaN，
// 不得因为 NaN 被序列化为 null 而在解码时抛 "type must be number"。
TEST(ProtocolCodec, CommandInsertUnknownPriceRoundtrip) {
    CommandBody in;
    in.op                   = CommandOp::InsertOrder;
    in.rid                  = "rid_mkt";
    in.insert.instrument_id = "SHFE.rb2610";
    in.insert.direction     = Direction::BUY;
    in.insert.offset        = Offset::OPEN;
    in.insert.price_type    = PriceType::ANY; // 市价单
    in.insert.limit_price   = kUnknownDouble; // NaN
    in.insert.volume        = 1;

    auto out = roundtrip_json(in);
    EXPECT_EQ(out.op, CommandOp::InsertOrder);
    EXPECT_EQ(out.insert.price_type, PriceType::ANY);
    EXPECT_FALSE(std::isfinite(out.insert.limit_price)); // 往返后仍为"未知价"
    EXPECT_EQ(out.insert.volume, 1u);

    // 同样验证 MessagePack 路径。
    ReceivedFrame fm{FrameType::Command,
                     FrameEncoding::MessagePack,
                     encode_body(in, FrameEncoding::MessagePack)};
    auto          om = decode_as<FrameType::Command>(fm);
    EXPECT_FALSE(std::isfinite(om.insert.limit_price));
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

TEST(ProtocolCodec, MarketDataPushKindRoundtrip) {
    MarketDataPushBody in;
    in.kind  = MdPushKind::Kline;
    in.data  = {{"klines", json::object()}};
    auto out = roundtrip_json(in);
    EXPECT_EQ(out.kind, MdPushKind::Kline);
    EXPECT_EQ(out.data, in.data);

    // 旧格式兼容：裸 diff（无 kind/data 包裹）应被识别为 Diff 且整体作为 data。
    ReceivedFrame legacy{FrameType::MarketDataPush, FrameEncoding::Json, [&] {
                             auto d = json{{"quotes", {{"SHFE.rb2610", {{"last_price", 1.0}}}}}};
                             auto s = d.dump();
                             return std::vector<uint8_t>(s.begin(), s.end());
                         }()};
    auto          leg = decode_body<MarketDataPushBody>(legacy);
    EXPECT_EQ(leg.kind, MdPushKind::Diff);
    EXPECT_DOUBLE_EQ(leg.data["quotes"]["SHFE.rb2610"]["last_price"].get<double>(), 1.0);
}

TEST(ProtocolCodec, TradePushKindRoundtrip) {
    TradePushBody in;
    in.kind  = TradePushKind::Diff;
    in.data  = {{"trade", json::object()}};
    auto out = roundtrip_json(in);
    EXPECT_EQ(out.kind, TradePushKind::Diff);
    EXPECT_EQ(out.data, in.data);
}

TEST(ProtocolCodec, GatewayEventRoundtrip) {
    GatewayEventBody in{GatewayEventKind::Error, "conn refused"};
    auto             out = roundtrip_json(in);
    EXPECT_EQ(out.kind, GatewayEventKind::Error);
    EXPECT_EQ(out.text, "conn refused");

    // 通过 decode_as<GatewayEvent> 校验映射已从 OpaqueBody 改为强类型。
    ReceivedFrame f{FrameType::GatewayEvent,
                    FrameEncoding::Json,
                    encode_body(GatewayEventBody{GatewayEventKind::Disconnected, "bye"})};
    auto          ev = decode_as<FrameType::GatewayEvent>(f);
    EXPECT_EQ(ev.kind, GatewayEventKind::Disconnected);
    EXPECT_EQ(ev.text, "bye");
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
