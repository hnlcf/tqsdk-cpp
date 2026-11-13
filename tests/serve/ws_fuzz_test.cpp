#include "tqsdk/pch.h"

#include "tqsdk/serve/ws_protocol.h"

#include "gtest/gtest.h"

#include <random>

namespace tqsdk {
namespace serve {
namespace {

TEST(WsFuzz, RandomClientTextNeverCrashes) {
    std::mt19937                          rng(0x51ee7);
    std::uniform_int_distribution<int>    byte_dist(0, 255);
    std::uniform_int_distribution<size_t> len_dist(0, 512);
    for (int iteration = 0; iteration < 1200; ++iteration) {
        std::string text(len_dist(rng), '\0');
        for (auto& ch : text) {
            ch = static_cast<char>(byte_dist(rng));
        }
        EXPECT_NO_THROW({
            try {
                (void)parse_ws_client_message(text);
            } catch (const std::exception&) {
            }
        });
        EXPECT_NO_THROW({
            try {
                (void)parse_ws_server_message(text);
            } catch (const std::exception&) {
            }
        });
    }
}

TEST(WsFuzz, DeeplyNestedInputIsRejectedWithoutCrash) {
    std::string text(128, '[');
    EXPECT_FALSE(parse_ws_client_message(text));
    EXPECT_FALSE(parse_ws_server_message(text));
}

} // namespace
} // namespace serve
} // namespace tqsdk
