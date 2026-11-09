#include "tqsdk/pch.h"

#include "tqsdk/auth/dynamic_endpoint.h"

#include <gtest/gtest.h>

namespace {

using namespace tqsdk;

TEST(DynamicEndpoint, ParsesWsAndWssUrls) {
    auto secure = Endpoint::parse("wss://md.example.com/t/md/front?token=x");
    ASSERT_TRUE(secure.has_value());
    EXPECT_EQ(secure->scheme, "wss");
    EXPECT_EQ(secure->host, "md.example.com");
    EXPECT_EQ(secure->port, 443);
    EXPECT_EQ(secure->path, "/t/md/front?token=x");

    auto plain = Endpoint::parse("ws://127.0.0.1:8765/stream");
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(plain->port, 8765);
    EXPECT_EQ(plain->path, "/stream");
    EXPECT_EQ(plain->to_string(), "ws://127.0.0.1:8765/stream");
}

TEST(DynamicEndpoint, RejectsMalformedUrls) {
    EXPECT_FALSE(Endpoint::parse("").has_value());
    EXPECT_FALSE(Endpoint::parse("md.example.com/path").has_value());
    EXPECT_FALSE(Endpoint::parse("wss:///path").has_value());
    EXPECT_FALSE(Endpoint::parse("wss://host:99999/path").has_value());
    EXPECT_FALSE(Endpoint::parse("wss://host:abc/path").has_value());
}

} // namespace
