#include "tqsdk/pch.h"

#include "tqsdk/util/json_value.h"

using namespace tqsdk;
using json = nlohmann::json;

TEST(JsonValue, NumbersAcceptOnlyStrictValues) {
    EXPECT_DOUBLE_EQ(json_value::number(json(1.5)).value(), 1.5);
    EXPECT_DOUBLE_EQ(json_value::number(json(" 1.5 ")).value(), 1.5);
    EXPECT_DOUBLE_EQ(json_value::number(json("-2.5e2")).value(), -250.0);
    EXPECT_FALSE(json_value::number(json("1.5x")));
    EXPECT_FALSE(json_value::number(json("nan")));
    EXPECT_FALSE(json_value::number(json("inf")));
    EXPECT_FALSE(json_value::number(json("")));
    EXPECT_FALSE(json_value::number(json(nullptr)));
    EXPECT_FALSE(json_value::number(json::array()));
}

TEST(JsonValue, IntegersRejectOverflowAndFraction) {
    EXPECT_EQ(json_value::integer(json(42)).value(), 42);
    EXPECT_EQ(json_value::integer(json(42.0)).value(), 42);
    EXPECT_EQ(json_value::integer(json("42")).value(), 42);
    EXPECT_EQ(json_value::integer(json("+42")).value(), 42);
    EXPECT_FALSE(json_value::integer(json(42.5)));
    EXPECT_FALSE(json_value::integer(json("42x")));
    EXPECT_FALSE(json_value::integer(json("9223372036854775808")));
    EXPECT_FALSE(json_value::integer(json(1e100)));

    EXPECT_EQ(json_value::unsigned_integer(json(42)).value(), 42u);
    EXPECT_EQ(json_value::unsigned_integer(json("18446744073709551615")).value(),
              std::numeric_limits<uint64_t>::max());
    EXPECT_FALSE(json_value::unsigned_integer(json(-1)));
    EXPECT_FALSE(json_value::unsigned_integer(json("-1")));
    EXPECT_FALSE(json_value::unsigned_integer(json("18446744073709551616")));
    EXPECT_FALSE(json_value::unsigned_integer(json(1e100)));
}

TEST(JsonValue, BooleansSupportProtocolForms) {
    EXPECT_TRUE(json_value::boolean(json(true)).value());
    EXPECT_FALSE(json_value::boolean(json(false)).value());
    EXPECT_TRUE(json_value::boolean(json(1)).value());
    EXPECT_FALSE(json_value::boolean(json(0)).value());
    EXPECT_TRUE(json_value::boolean(json("true")).value());
    EXPECT_FALSE(json_value::boolean(json("false")).value());
    EXPECT_TRUE(json_value::boolean(json(" yes ")).value());
    EXPECT_FALSE(json_value::boolean(json("no")).value());
    EXPECT_FALSE(json_value::boolean(json(2)));
    EXPECT_FALSE(json_value::boolean(json("TRUE-ish")));
}

TEST(JsonValue, StringDoesNotImplicitlyConvertNumbers) {
    EXPECT_EQ(json_value::string(json("abc")).value(), "abc");
    EXPECT_FALSE(json_value::string(json(42)));
    EXPECT_EQ(json_value::string_or(json{{"name", 42}}, "name", "fallback"), "fallback");
    EXPECT_EQ(json_value::string_or(json{{"name", "ok"}}, "name", "fallback"), "ok");
}

TEST(JsonValue, ObjectAccessorsUseFallbacks) {
    json object{{"price", "12.5"}, {"count", "7"}, {"expired", "yes"}, {"name", 123}};
    EXPECT_DOUBLE_EQ(json_value::number_or(object, "price"), 12.5);
    EXPECT_EQ(json_value::integer_or(object, "count"), 7);
    EXPECT_TRUE(json_value::boolean_or(object, "expired"));
    EXPECT_EQ(json_value::string_or(object, "name", "default"), "default");
    EXPECT_EQ(json_value::number_or(object, "missing", 3.5), 3.5);
}

TEST(JsonValue, QueryParsersAreStrict) {
    EXPECT_EQ(json_value::parse_unsigned(" +42 ").value(), 42u);
    EXPECT_FALSE(json_value::parse_unsigned("-1"));
    EXPECT_FALSE(json_value::parse_unsigned("12x"));
    EXPECT_TRUE(json_value::parse_boolean("TRUE").value());
    EXPECT_FALSE(json_value::parse_boolean("false").value());
    EXPECT_FALSE(json_value::parse_boolean("maybe"));
}
