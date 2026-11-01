#include "tqsdk/pch.h"

#include "tqsdk/msg.h"

#include "support/test_helpers.h"

using json    = nlohmann::json;
namespace msg = tqsdk::msg;

// ============================================================
// 额外测试类型：验证 optional 字段逻辑（头文件中没有可选字段）
// ============================================================
namespace testmsg {

struct OptMsg {
    static constexpr const char* aid = "opt_msg";

    std::string required_field;
    std::string opt_field = "default_value";

    static auto fields() {
        using Self = OptMsg;
        return std::make_tuple(
            ::tqsdk::msg::detail::field("required_field", &Self::required_field),
            ::tqsdk::msg::detail::field("opt_field", &Self::opt_field, /*optional=*/true));
    }
};

} // namespace testmsg

DEFINE_JSON_MESSAGE(testmsg::OptMsg)

// ============================================================
// 1. 序列化
// ============================================================

TEST(MsgSerialize, SetChart) {
    msg::SetChart m;
    m.chart_id   = "abcd123";
    m.ins_list   = "SHFE.cu1701";
    m.duration   = 180000000000LL; // 超过 int32，验证 int64 不截断
    m.view_width = 500;

    json j = m;

    EXPECT_EQ(j["aid"], "set_chart");
    EXPECT_EQ(j["chart_id"], "abcd123");
    EXPECT_EQ(j["ins_list"], "SHFE.cu1701");
    EXPECT_EQ(j["duration"].get<int64_t>(), 180000000000LL);
    EXPECT_EQ(j["view_width"], 500);
}

TEST(MsgSerialize, SubscribeQuote) {
    msg::SubscribeQuote m{"SHFE.cu1612,CFFEX.IF1701"};
    json                j = m;

    EXPECT_EQ(j["aid"], "subscribe_quote");
    EXPECT_EQ(j["ins_list"], "SHFE.cu1612,CFFEX.IF1701");
}

TEST(MsgSerialize, ReqLogin) {
    msg::ReqLogin m{"aaa", "43214", "abcd123"};
    json          j = m;

    EXPECT_EQ(j["aid"], "req_login");
    EXPECT_EQ(j["bid"], "aaa");
    EXPECT_EQ(j["user_name"], "43214");
    EXPECT_EQ(j["password"], "abcd123");
}

TEST(MsgSerialize, ClearsStaleContent) {
    // message_to_json 内部 j = json::object() 应清空已有键
    json j;
    j["stale"] = 42;
    j["aid"]   = "old_value";

    j = msg::ReqLogin{"a", "u", "p"};

    EXPECT_FALSE(j.contains("stale"));
    EXPECT_EQ(j["aid"], "req_login");
}

// ============================================================
// 2. 反序列化（正常路径）
// ============================================================

TEST(MsgDeserialize, SetChart) {
    auto j = json::parse(R"({
        "aid": "set_chart",
        "chart_id": "abcd123",
        "ins_list": "SHFE.cu1701",
        "duration": 180000000000,
        "view_width": 500
    })");

    auto m = j.get<msg::SetChart>();
    EXPECT_EQ(m.chart_id, "abcd123");
    EXPECT_EQ(m.ins_list, "SHFE.cu1701");
    EXPECT_EQ(m.duration, 180000000000LL);
    EXPECT_EQ(m.view_width, 500);
}

TEST(MsgDeserialize, SubscribeQuote) {
    auto j = json::parse(R"({"aid":"subscribe_quote","ins_list":"SHFE.cu1612,CFFEX.IF1701"})");
    auto m = j.get<msg::SubscribeQuote>();
    EXPECT_EQ(m.ins_list, "SHFE.cu1612,CFFEX.IF1701");
}

TEST(MsgDeserialize, ReqLogin) {
    auto j = json::parse(R"({
        "aid": "req_login", "bid": "aaa", "user_name": "43214", "password": "abcd123"
    })");
    auto m = j.get<msg::ReqLogin>();
    EXPECT_EQ(m.bid, "aaa");
    EXPECT_EQ(m.user_name, "43214");
    EXPECT_EQ(m.password, "abcd123");
}

TEST(MsgDeserialize, IgnoresUnknownFields) {
    // nlohmann 默认忽略未知字段，应保持向前兼容
    auto j = json::parse(R"({
        "aid": "req_login", "bid": "a", "user_name": "u", "password": "p",
        "future_field": {"nested": true}, "another": 123
    })");
    EXPECT_NO_THROW({
        auto m = j.get<msg::ReqLogin>();
        EXPECT_EQ(m.bid, "a");
    });
}

// ============================================================
// 3. 往返一致性
// ============================================================

TEST(MsgRoundTrip, SetChart) {
    msg::SetChart in;
    in.chart_id   = "c1";
    in.ins_list   = "SHFE.cu1701,SHFE.cu1702";
    in.duration   = 86400000000000LL; // 日线
    in.view_width = 800;

    json j   = in;
    auto out = j.get<msg::SetChart>();

    EXPECT_EQ(out.chart_id, in.chart_id);
    EXPECT_EQ(out.ins_list, in.ins_list);
    EXPECT_EQ(out.duration, in.duration);
    EXPECT_EQ(out.view_width, in.view_width);
}

TEST(MsgRoundTrip, SubscribeQuote) {
    msg::SubscribeQuote in{"SHFE.cu1612,CFFEX.IF1701"};
    auto                out = json(in).get<msg::SubscribeQuote>();
    EXPECT_EQ(out.ins_list, in.ins_list);
}

TEST(MsgRoundTrip, ReqLogin) {
    msg::ReqLogin in{"aaa", "43214", "abcd123"};
    auto          out = json(in).get<msg::ReqLogin>();
    EXPECT_EQ(out.bid, in.bid);
    EXPECT_EQ(out.user_name, in.user_name);
    EXPECT_EQ(out.password, in.password);
}

// ============================================================
// 4. 错误路径：aid 相关
// ============================================================

TEST(MsgDeserializeError, MissingAid) {
    auto j = json::parse(R"({"chart_id":"c","ins_list":"i","duration":0,"view_width":1})");
    EXPECT_THROW(j.get<msg::SetChart>(), std::invalid_argument);
}

TEST(MsgDeserializeError, AidNotString) {
    auto j =
        json::parse(R"({"aid":123,"chart_id":"c","ins_list":"i","duration":0,"view_width":1})");
    EXPECT_THROW(j.get<msg::SetChart>(), std::invalid_argument);
}

TEST(MsgDeserializeError, AidMismatch) {
    // 用 req_login 的报文去解析 SetChart
    auto j = json::parse(R"({"aid":"req_login","bid":"a","user_name":"u","password":"p"})");
    EXPECT_THROW(j.get<msg::SetChart>(), std::invalid_argument);
}

TEST(MsgDeserializeError, NonObjectInput) {
    EXPECT_THROW(json::array().get<msg::ReqLogin>(), std::invalid_argument);
    EXPECT_THROW(json(nullptr).get<msg::ReqLogin>(), std::invalid_argument);
    EXPECT_THROW(json(42).get<msg::ReqLogin>(), std::invalid_argument);
    EXPECT_THROW(json("req_login").get<msg::ReqLogin>(), std::invalid_argument);
}

// ============================================================
// 5. 错误路径：字段相关
// ============================================================

TEST(MsgDeserializeError, MissingRequiredField) {
    auto j = json::parse(R"({
        "aid": "set_chart", "ins_list": "i", "duration": 0, "view_width": 1
    })");
    // read_field 对缺失必填字段抛 std::out_of_range
    EXPECT_THROW(j.get<msg::SetChart>(), std::out_of_range);
}

TEST(MsgDeserializeError, WrongFieldType) {
    auto j = json::parse(R"({
        "aid": "set_chart", "chart_id": "c", "ins_list": "i",
        "duration": "not_a_number", "view_width": 1
    })");
    // get_to<int64_t> 对字符串抛 nlohmann::json::type_error
    EXPECT_THROW(j.get<msg::SetChart>(), nlohmann::json::type_error);
}

TEST(MsgDeserializeError, NegativeViewWidthIsNotTypeError) {
    // 类型正确、值不合理不属于序列化层职责，应正常解析
    auto j = json::parse(R"({
        "aid": "set_chart", "chart_id": "c", "ins_list": "i",
        "duration": 0, "view_width": -1
    })");
    EXPECT_NO_THROW({
        auto m = j.get<msg::SetChart>();
        EXPECT_EQ(m.view_width, -1);
    });
}

// ============================================================
// 6. 可选字段（custom test type）
// ============================================================

TEST(OptionalField, MissingOptionalFieldUsesDefault) {
    auto j = json::parse(R"({"aid":"opt_msg","required_field":"hello"})");
    auto m = j.get<testmsg::OptMsg>();
    EXPECT_EQ(m.required_field, "hello");
    EXPECT_EQ(m.opt_field, "default_value"); // 缺失时保持默认
}

TEST(OptionalField, PresentOptionalFieldIsRead) {
    auto j = json::parse(R"({"aid":"opt_msg","required_field":"hello","opt_field":"world"})");
    auto m = j.get<testmsg::OptMsg>();
    EXPECT_EQ(m.opt_field, "world");
}

TEST(OptionalField, MissingRequiredFieldStillThrows) {
    auto j = json::parse(R"({"aid":"opt_msg","opt_field":"world"})");
    EXPECT_THROW(j.get<testmsg::OptMsg>(), std::out_of_range);
}

// ============================================================
// 7. 多类型互不干扰
// ============================================================

TEST(MsgMultiType, SequentialSerializeIndependently) {
    json a = msg::SetChart{"c", "i", 1, 2};
    json b = msg::SubscribeQuote{"X.Y"};
    json c = msg::ReqLogin{"bid", "user", "pass"};

    EXPECT_EQ(a["aid"], "set_chart");
    EXPECT_EQ(b["aid"], "subscribe_quote");
    EXPECT_EQ(c["aid"], "req_login");

    // 反序列化也不会串类型
    EXPECT_EQ(a.get<msg::SetChart>().chart_id, "c");
    EXPECT_EQ(b.get<msg::SubscribeQuote>().ins_list, "X.Y");
    EXPECT_EQ(c.get<msg::ReqLogin>().user_name, "user");
}

// ============================================================
// 8. 端到端：原始 JSON 文本 -> 结构体 -> JSON 文本
// ============================================================

TEST(MsgEndToEnd, RawTextRoundTrip) {
    const std::string raw = R"({
        "aid": "set_chart",
        "chart_id": "abcd123",
        "ins_list": "SHFE.cu1701",
        "duration": 180000000000,
        "view_width": 500
    })";

    auto m   = json::parse(raw).get<msg::SetChart>();
    json out = m;

    EXPECT_EQ(out["aid"], "set_chart");
    EXPECT_EQ(out["duration"].get<int64_t>(), 180000000000LL);
    EXPECT_EQ(out["view_width"], 500);
}
