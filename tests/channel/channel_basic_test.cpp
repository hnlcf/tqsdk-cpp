#include "tqsdk/pch.h"

#include "tqsdk/channel/channel.h"

#include "support/test_helpers.h"

using namespace tqsdk;
namespace asio = boost::asio;
using namespace tqsdk::test;

TEST(ChannelBasic, SendThenReceive) {
    asio::io_context ctx;
    Channel<int>     ch(ctx.get_executor());

    std::optional<int> got;
    run_coro(ctx, [&]() -> asio::awaitable<void> {
        co_await ch.async_send(42);
        got = co_await ch.async_receive();
        co_return;
    }());

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, 42);
    EXPECT_EQ(ch.size(), 0u);
}

TEST(ChannelBasic, ReceiveFirstThenSend) {
    asio::io_context ctx;
    Channel<int>     ch(ctx.get_executor());

    std::optional<int> got;
    bool               receive_started = false;
    bool               receive_done    = false;

    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            receive_started = true;
            got             = co_await ch.async_receive();
            receive_done    = true;
            co_return;
        },
        asio::detached);

    expect_pending(ctx, [&] { return receive_done; });
    EXPECT_TRUE(receive_started);

    run_coro(ctx, [&]() -> asio::awaitable<void> {
        co_await ch.async_send(7);
        co_return;
    }());

    EXPECT_TRUE(receive_done);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, 7);
}

TEST(ChannelBasic, FifoOrder) {
    asio::io_context ctx;
    Channel<int>     ch(ctx.get_executor());

    std::vector<int> got;
    run_coro(ctx, [&]() -> asio::awaitable<void> {
        co_await ch.async_send(1);
        co_await ch.async_send(2);
        co_await ch.async_send(3);
        for (int i = 0; i < 3; ++i) {
            auto v = co_await ch.async_receive();
            got.push_back(*v);
        }
        co_return;
    }());

    EXPECT_EQ(got, (std::vector<int>{1, 2, 3}));
}

TEST(ChannelBasic, MovesNonCopyablePayload) {
    asio::io_context              ctx;
    Channel<std::unique_ptr<int>> ch(ctx.get_executor());

    std::unique_ptr<int> got;
    run_coro(ctx, [&]() -> asio::awaitable<void> {
        co_await ch.async_send(std::make_unique<int>(99));
        auto v = co_await ch.async_receive();
        got    = std::move(*v);
        co_return;
    }());

    ASSERT_NE(got, nullptr);
    EXPECT_EQ(*got, 99);
}
