#include "tqsdk/pch.h"

#include "tqsdk/types.h"
#include "tqsdk/util/serde.h"

#include "support/test_helpers.h"

using namespace tqsdk;

struct TestOrder {
    int         id;
    OrderStatus status;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TestOrder, id, status)

// ---- 4.1 序列化 ----
TEST(SerdeEnumMacro, SerializeAlive) {
    nlohmann::json j = OrderStatus::ALIVE;
    EXPECT_TRUE(j.is_string());
    EXPECT_EQ(j.get<std::string>(), "ALIVE");
}

TEST(SerdeEnumMacro, SerializeFinished) {
    nlohmann::json j = OrderStatus::FINISHED;
    EXPECT_EQ(j.get<std::string>(), "FINISHED");
}

// ---- 4.2 反序列化 ----
TEST(SerdeEnumMacro, DeserializeAlive) {
    auto s = nlohmann::json("ALIVE").get<OrderStatus>();
    EXPECT_EQ(s, OrderStatus::ALIVE);
}

TEST(SerdeEnumMacro, DeserializeFinished) {
    auto s = nlohmann::json("FINISHED").get<OrderStatus>();
    EXPECT_EQ(s, OrderStatus::FINISHED);
}

// ---- 4.3 往返一致性（遍历所有枚举值） ----
TEST(SerdeEnumMacro, RoundTripAllValues) {
    for (auto v : magic_enum::enum_values<OrderStatus>()) {
        nlohmann::json j        = v;                    // 序列化
        auto           restored = j.get<OrderStatus>(); // 反序列化
        EXPECT_EQ(restored, v) << "round-trip failed for " << magic_enum::enum_name(v);
    }
}

// ---- 4.4 无效枚举值：序列化应抛异常 ----
TEST(SerdeEnumMacro, SerializeInvalidValueThrows) {
    auto invalid = static_cast<OrderStatus>(99);
    EXPECT_THROW({ nlohmann::json j = invalid; }, std::invalid_argument);
}

// ---- 4.5 无效字符串：反序列化应抛异常 ----
TEST(SerdeEnumMacro, DeserializeUnknownStringThrows) {
    EXPECT_THROW(nlohmann::json("UNKNOWN").get<OrderStatus>(), std::invalid_argument);
}

// ---- 4.6 大小写敏感：小写不应被接受 ----
TEST(SerdeEnumMacro, DeserializeIsCaseSensitive) {
    EXPECT_THROW(nlohmann::json("alive").get<OrderStatus>(), std::invalid_argument);
}

// ---- 4.7 非字符串输入：反序列化应抛异常 ----
TEST(SerdeEnumMacro, DeserializeNonStringThrows) {
    // 数字（模拟旧格式：底层 uint8_t 值）
    EXPECT_THROW(nlohmann::json(0).get<OrderStatus>(), std::invalid_argument);

    // 对象
    EXPECT_THROW(nlohmann::json::object().get<OrderStatus>(), std::invalid_argument);

    // null
    EXPECT_THROW(nlohmann::json(nullptr).get<OrderStatus>(), std::invalid_argument);
}

// ---- 4.8 底层类型不被改变（仍是 uint8_t，且序列化输出为字符串） ----
TEST(SerdeEnumMacro, UnderlyingTypePreserved) {
    static_assert(std::is_same_v<std::underlying_type_t<OrderStatus>, uint8_t>,
                  "underlying type must remain uint8_t");

    // 确认 JSON 中存的是字符串而非数字
    nlohmann::json j = OrderStatus::ALIVE;
    EXPECT_FALSE(j.is_number());
    EXPECT_TRUE(j.is_string());
}

// ---- 4.9 容器中的枚举 ----
TEST(SerdeEnumMacro, VectorRoundTrip) {
    std::vector<OrderStatus> input = {
        OrderStatus::ALIVE, OrderStatus::FINISHED, OrderStatus::ALIVE};

    nlohmann::json j = input;
    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 3u);
    EXPECT_EQ(j[0], "ALIVE");
    EXPECT_EQ(j[1], "FINISHED");
    EXPECT_EQ(j[2], "ALIVE");

    auto output = j.get<std::vector<OrderStatus>>();
    EXPECT_EQ(output, input);
}

// ---- 4.10 嵌套结构体 ----
TEST(SerdeEnumMacro, NestedStructRoundTrip) {
    TestOrder o{42, OrderStatus::FINISHED};

    nlohmann::json j = o;
    EXPECT_EQ(j["id"], 42);
    EXPECT_EQ(j["status"], "FINISHED"); // 枚举被序列化为字符串

    auto restored = j.get<TestOrder>();
    EXPECT_EQ(restored.id, 42);
    EXPECT_EQ(restored.status, OrderStatus::FINISHED);
}

// ---- 4.11 解析实际 JSON 文本 ----
TEST(SerdeEnumMacro, ParseFromRawText) {
    auto j = nlohmann::json::parse(R"({"id":7,"status":"ALIVE"})");
    auto o = j.get<TestOrder>();
    EXPECT_EQ(o.id, 7);
    EXPECT_EQ(o.status, OrderStatus::ALIVE);
}
