#include "tqsdk/pch.h"

#include "tqsdk/util/spawn_logged.h"

#include "support/test_helpers.h"

using namespace tqsdk;
namespace asio = boost::asio;

TEST(SpawnLogged, SwallowsException) {
    asio::io_context ctx;
    // 协程抛异常,spawn_logged 应捕获而不传播
    tqsdk::co_spawn_logged(
        ctx.get_executor(),
        []() -> asio::awaitable<void> {
            throw std::runtime_error("boom");
            co_return;
        },
        "test.throw");
    EXPECT_NO_THROW(ctx.run());
}

TEST(SpawnLogged, NormalCompletion) {
    asio::io_context ctx;
    bool             ran = false;
    tqsdk::co_spawn_logged(
        ctx.get_executor(),
        [&]() -> asio::awaitable<void> {
            ran = true;
            co_return;
        },
        "test.ok");
    ctx.run();
    EXPECT_TRUE(ran);
}
