#include "tqsdk/pch.h"

#include "tqsdk/channel/channel.h"

#include "support/test_helpers.h"

using namespace tqsdk;
namespace asio = boost::asio;
using namespace tqsdk::test;

TEST(ChannelClose, ReceiveReturnsNulloptAfterClose) {
    asio::io_context ctx;
    Channel<int>     ch(ctx.get_executor());
    ch.close();

    std::optional<int> got  = 42; // 非空初始值，验证会被覆盖
    bool               done = false;
    run_coro(ctx, [&]() -> asio::awaitable<void> {
        got  = co_await ch.async_receive();
        done = true;
        co_return;
    }());

    EXPECT_TRUE(done);
    EXPECT_FALSE(got.has_value());
}

TEST(ChannelClose, DrainsQueuedDataBeforeNullopt) {
    asio::io_context ctx;
    Channel<int>     ch(ctx.get_executor());

    run_coro(ctx, [&]() -> asio::awaitable<void> {
        co_await ch.async_send(1);
        co_await ch.async_send(2);
        co_return;
    }());

    ch.close();

    std::vector<int> got;
    run_coro(ctx, [&]() -> asio::awaitable<void> {
        while (auto v = co_await ch.async_receive()) {
            got.push_back(*v);
        }
        co_return;
    }());

    EXPECT_TRUE(ch.is_closed());
    EXPECT_EQ(ch.size(), 0u);

    EXPECT_EQ(got, (std::vector<int>{1, 2}));
}

TEST(ChannelClose, SendAfterCloseThrows) {
    asio::io_context ctx;
    Channel<int>     ch(ctx.get_executor());
    ch.close();

    bool threw = false;
    try {
        run_coro(ctx, [&]() -> asio::awaitable<void> {
            co_await ch.async_send(1);
            co_return;
        }());
    } catch (const boost::system::system_error& e) {
        threw = true;
        EXPECT_EQ(e.code(), asio::error::operation_aborted);
    }
    EXPECT_TRUE(threw);
}

TEST(ChannelClose, WakesAllPendingReceivers) {
    asio::io_context ctx;
    Channel<int>     ch(ctx.get_executor());

    std::vector<int> done_count(3, 0);
    for (int i = 0; i < 3; ++i) {
        asio::co_spawn(
            ctx,
            [&, i]() -> asio::awaitable<void> {
                auto v        = co_await ch.async_receive();
                done_count[i] = v.has_value() ? 1 : 2; // 1=got, 2=nullopt
                co_return;
            },
            asio::detached);
    }

    pump(ctx, 20);
    EXPECT_EQ(std::count(done_count.begin(), done_count.end(), 0), 3);

    ch.close();
    pump(ctx, 20);

    EXPECT_EQ(std::count(done_count.begin(), done_count.end(), 2), 3);
}

TEST(ChannelClose, WakesAllPendingSenders) {
    asio::io_context ctx;
    Channel<int>     ch(ctx.get_executor(), /*capacity=*/1);

    run_coro(ctx, [&]() -> asio::awaitable<void> {
        co_await ch.async_send(0); // 占满
        co_return;
    }());

    std::vector<int> results(2, 0); // 0=pending, 1=done, 2=aborted
    for (int i = 0; i < 2; ++i) {
        asio::co_spawn(
            ctx,
            [&, i]() -> asio::awaitable<void> {
                try {
                    co_await ch.async_send(i + 1);
                    results[i] = 1;
                } catch (const boost::system::system_error& e) {
                    results[i] = (e.code() == asio::error::operation_aborted) ? 2 : 3;
                }
                co_return;
            },
            asio::detached);
    }

    pump(ctx, 20);
    EXPECT_EQ(std::count(results.begin(), results.end(), 0), 2);

    ch.close();
    pump(ctx, 20);

    EXPECT_EQ(std::count(results.begin(), results.end(), 2), 2);
}

TEST(ChannelClose, Idempotent) {
    asio::io_context ctx;
    Channel<int>     ch(ctx.get_executor());
    ch.close();
    ch.close();
    ch.close();
    EXPECT_TRUE(ch.is_closed());
}

TEST(ChannelClose, MultipleReceiversOneItem) {
    // 语义校验：一次 send 只唤醒一个 receiver
    asio::io_context ctx;
    Channel<int>     ch(ctx.get_executor());

    int v1 = 0, v2 = 0;
    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            auto v = co_await ch.async_receive();
            v1     = v.has_value() ? *v : -1;
            co_return;
        },
        asio::detached);
    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            auto v = co_await ch.async_receive();
            v2     = v.has_value() ? *v : -1;
            co_return;
        },
        asio::detached);

    pump(ctx, 20);
    EXPECT_EQ(v1, 0);
    EXPECT_EQ(v2, 0);

    run_coro(ctx, [&]() -> asio::awaitable<void> {
        co_await ch.async_send(99);
        co_return;
    }());

    pump(ctx, 20);
    // 恰好一个拿到，另一个仍挂起（v 保持 0）
    int got = (v1 == 99) + (v2 == 99);
    EXPECT_EQ(got, 1);
    int pending = (v1 == 0) + (v2 == 0);
    EXPECT_EQ(pending, 1);

    ch.close(); // 唤醒剩下的
    pump(ctx, 20);
    EXPECT_EQ((v1 == -1) + (v2 == -1), 1);
}
