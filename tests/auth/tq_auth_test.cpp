#include "tqsdk/auth/tq_auth.h"
#include "tqsdk/config/tqapi_factory.h"

#include <gtest/gtest.h>

namespace {

using namespace tqsdk;

TEST(TqAuth, DecodesJwtPayloadWithoutSignatureVerification) {
    EXPECT_EQ(base64url_decode("eyJleHAiOjE3MDAwMDAwMDB9"), "{\"exp\":1700000000}");
    EXPECT_EQ(base64url_decode("bad!"), "");
}

TEST(TqAuth, DisabledAuthDoesNotCreateBearerHeader) {
    TqAuth auth{TqAuth::Config{}};
    EXPECT_FALSE(auth.login());
    EXPECT_TRUE(auth.authorization_header().empty());
}

TEST(TqAuth, ParsesNodeAuthConfig) {
    const auto cfg = parse_node_config(R"(
[tq_auth]
enabled = true
user_name = "tq_user"
password = "tq_password"
auth_url = "https://auth.example.test/"
request_timeout_sec = 7
refresh_margin_sec = 11
)");
    EXPECT_TRUE(cfg.tq_auth.enabled);
    EXPECT_EQ(cfg.tq_auth.user_name, "tq_user");
    EXPECT_EQ(cfg.tq_auth.auth_url, "https://auth.example.test/");
    EXPECT_EQ(cfg.tq_auth.request_timeout.count(), 7);
    EXPECT_EQ(cfg.tq_auth.refresh_margin.count(), 11);
}

} // namespace
