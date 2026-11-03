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

TEST(ChannelBasic, TrySendDropsOldestWhenFull) {
    asio::io_context ctx;
    Channel<int>     ch(ctx.get_executor(), 2);

    EXPECT_TRUE(ch.try_send(1));
    EXPECT_TRUE(ch.try_send(2));
    EXPECT_TRUE(ch.try_send(3)); // 满：丢弃最旧的 1
    EXPECT_EQ(ch.size(), 2u);

    std::vector<int> got;
    run_coro(ctx, [&]() -> asio::awaitable<void> {
        got.push_back(*co_await ch.async_receive());
        got.push_back(*co_await ch.async_receive());
        co_return;
    }());
    EXPECT_EQ(got, (std::vector<int>{2, 3}));

    ch.close();
    EXPECT_FALSE(ch.try_send(4));
}

TEST(ChannelBasic, PostSendFromWorkerThreads) {
    asio::io_context ctx;
    auto             strand = asio::make_strand(ctx);
    Channel<int>     ch(strand);

    constexpr int kThreads   = 4;
    constexpr int kPerThread = 100;
    constexpr int kTotal     = kThreads * kPerThread;

    int       received = 0;
    long long sum      = 0;
    bool      done     = false;
    asio::co_spawn(
        strand,
        [&]() -> asio::awaitable<void> {
            while (received < kTotal) {
                auto item = co_await ch.async_receive();
                if (!item) {
                    break;
                }
                ++received;
                sum += *item;
            }
            done = true;
            ch.close();
            co_return;
        },
        asio::detached);

    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&ch, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                ch.post_send(t * kPerThread + i);
            }
        });
    }
    for (auto& t : producers) {
        t.join();
    }
    ctx.run();

    EXPECT_TRUE(done);
    EXPECT_EQ(received, kTotal);
    EXPECT_EQ(sum, static_cast<long long>(kTotal - 1) * kTotal / 2);
}
