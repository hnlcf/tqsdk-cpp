#include "tqsdk/pch.h"

#include "tqsdk/channel/channel.h"

#include "support/test_helpers.h"

using namespace tqsdk;
namespace asio = boost::asio;
using namespace tqsdk::test;

TEST(ChannelBounded, UnboundedDoesNotBlock) {
    asio::io_context ctx;
    Channel<int>     ch(ctx.get_executor(), /*capacity=*/0);

    for (int i = 0; i < 10'000; ++i) {
        run_coro(ctx, [&]() -> asio::awaitable<void> {
            co_await ch.async_send(i);
            co_return;
        }());
    }
    EXPECT_EQ(ch.size(), 10'000u);
}

TEST(ChannelBounded, SendBlocksWhenFull) {
    asio::io_context ctx;
    Channel<int>     ch(ctx.get_executor(), /*capacity=*/2);

    // 填满
    run_coro(ctx, [&]() -> asio::awaitable<void> {
        co_await ch.async_send(1);
        co_await ch.async_send(2);
        co_return;
    }());
    EXPECT_EQ(ch.size(), 2u);

    // 第三次 send 必须挂起
    bool send3_done = false;
    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            co_await ch.async_send(3);
            send3_done = true;
            co_return;
        },
        asio::detached);

    expect_pending(ctx, [&] { return send3_done; });
    EXPECT_EQ(ch.size(), 2u); // 3 还没进去

    // 一个 receive 释放空间，3 应该被唤醒并进入
    run_coro(ctx, [&]() -> asio::awaitable<void> {
        auto v = co_await ch.async_receive();
        EXPECT_EQ(*v, 1);
        co_return;
    }());

    pump(ctx); // 让 send3 的协程恢复
    EXPECT_TRUE(send3_done);
    EXPECT_EQ(ch.size(), 2u); // 剩余 2, 3

    // 验证剩余顺序
    run_coro(ctx, [&]() -> asio::awaitable<void> {
        auto a = co_await ch.async_receive();
        auto b = co_await ch.async_receive();
        EXPECT_EQ(*a, 2);
        EXPECT_EQ(*b, 3);
        co_return;
    }());
}
