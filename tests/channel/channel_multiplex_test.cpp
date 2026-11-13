#include "tqsdk/pch.h"

#include "tqsdk/channel/channel.h"

#include "support/test_helpers.h"

using namespace tqsdk;
namespace asio = boost::asio;
using namespace tqsdk::test;

TEST(ChannelMux, ManyProducersManyConsumers) {
    asio::io_context ctx;
    Channel<int>     ch(ctx.get_executor(), /*capacity=*/16);

    constexpr int kProducers   = 4;
    constexpr int kPerProducer = 250;
    constexpr int kConsumers   = 4;
    constexpr int kTotal       = kProducers * kPerProducer;

    std::vector<int> received;
    received.reserve(kTotal);

    for (int c = 0; c < kConsumers; ++c) {
        asio::co_spawn(
            ctx,
            [&]() -> asio::awaitable<void> {
                while (true) {
                    auto v = co_await ch.async_receive();
                    if (!v) {
                        co_return;
                    }
                    received.push_back(*v);
                }
            },
            asio::detached);
    }

    std::atomic<int> producers_done{0};
    for (int p = 0; p < kProducers; ++p) {
        asio::co_spawn(
            ctx,
            [&, p]() -> asio::awaitable<void> {
                for (int i = 0; i < kPerProducer; ++i) {
                    co_await ch.async_send(p * kPerProducer + i);
                }
                if (++producers_done == kProducers) {
                    ch.close();
                }
                co_return;
            },
            asio::detached);
    }

    // 保证有界 channel 不会死锁；一直跑到所有协程结束
    while (producers_done < kProducers || !ch.is_closed()) {
        ctx.restart();
        ctx.run_for(std::chrono::milliseconds(50));
    }
    ctx.restart();
    ctx.run_for(std::chrono::milliseconds(50)); // 让消费者处理最后一个 close

    EXPECT_EQ(received.size(), (size_t)kTotal);
    std::sort(received.begin(), received.end());
    for (int i = 0; i < kTotal; ++i) {
        EXPECT_EQ(received[i], i);
    }
}
