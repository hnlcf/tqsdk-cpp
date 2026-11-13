#include "tqsdk/pch.h"

#include "tqsdk/ipc/protocol.h"

#include "gtest/gtest.h"

#include <random>

namespace tqsdk {
namespace ipc {
namespace {

std::vector<uint8_t> random_bytes(std::mt19937& rng, std::size_t max_len) {
    std::uniform_int_distribution<std::size_t> len_dist(0, max_len);
    std::uniform_int_distribution<int>         byte_dist(0, 255);
    std::vector<uint8_t>                       out(len_dist(rng));
    for (auto& byte : out) {
        byte = static_cast<uint8_t>(byte_dist(rng));
    }
    return out;
}

/// Deterministic malformed-input smoke test. This is intentionally not a
/// libFuzzer target: it runs in normal ctest/ASan/UBSan jobs and asserts that
/// every parser rejects bad input with an exception instead of crashing.
TEST(ProtocolFuzz, RandomBodiesNeverCrash) {
    std::mt19937 rng(0x5eed1234);
    for (int iteration = 0; iteration < 1500; ++iteration) {
        ReceivedFrame frame;
        frame.type     = FrameType::Hello;
        frame.encoding = (iteration & 1) ? FrameEncoding::Json : FrameEncoding::MessagePack;
        frame.payload  = random_bytes(rng, 384);

        EXPECT_NO_THROW({
            try {
                (void)decode_body<HelloBody>(frame);
            } catch (const std::exception&) {
            }
        });
        EXPECT_NO_THROW({
            try {
                (void)decode_body<SubscribeBody>(frame);
            } catch (const std::exception&) {
            }
        });
        EXPECT_NO_THROW({
            try {
                (void)decode_body<CommandBody>(frame);
            } catch (const std::exception&) {
            }
        });
        EXPECT_NO_THROW({
            try {
                (void)decode_body<ResumeBody>(frame);
            } catch (const std::exception&) {
            }
        });
    }
}

TEST(ProtocolFuzz, InvalidFrameKindsThrowWithoutCrash) {
    ReceivedFrame frame;
    frame.type     = static_cast<FrameType>(0xfe);
    frame.encoding = static_cast<FrameEncoding>(0xfe);
    frame.payload  = {0x7b, 0x7d}; // {}
    EXPECT_THROW(decode_body<HelloBody>(frame), std::exception);
}

} // namespace
} // namespace ipc
} // namespace tqsdk
