#include "tqsdk/pch.h"

#include "tqsdk/channel/channel.h"

namespace asio = boost::asio;
using namespace tqsdk;

TEST(Channel, SendReceive) {
    asio::io_context    ctx;
    tqsdk::Channel<int> ch(ctx.get_executor());
    bool                done = false;

    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            co_await ch.async_send(42);
            auto v = co_await ch.async_receive();
            EXPECT_TRUE(v.has_value());
            EXPECT_EQ(*v, 42);
            done = true;
            co_return;
        },
        asio::detached);

    ctx.run();
    EXPECT_TRUE(done);
}

TEST(Channel, CloseWakesReceiver) {
    asio::io_context    ctx;
    tqsdk::Channel<int> ch(ctx.get_executor());
    std::optional<int>  got = 0;

    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            got = co_await ch.async_receive();
            co_return;
        },
        asio::detached);

    // 让 receive 先挂起
    ctx.run_for(std::chrono::milliseconds(10));
    ch.close();
    ctx.run();
    EXPECT_FALSE(got.has_value());
}
