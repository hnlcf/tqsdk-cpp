#include "tqsdk/pch.h"

#include "tqsdk/gateway/tq/tq_md_gateway.h"
#include "tqsdk/tqapi.h"

#include "support/test_helpers.h"

using namespace tqsdk;

// 联网集成测试：连接真实天勤免费行情服务器。
// 默认跳过；设置环境变量 TQSDK_RUN_NETWORK=1 时启用。
namespace {

bool network_enabled() {
    const char* v = std::getenv("TQSDK_RUN_NETWORK");
    return v != nullptr && std::string(v) == "1";
}

} // namespace

// 真实连接 -> 订阅 -> 收到行情 diff -> get_quote 能读到 finite last_price。
TEST(TqMdNetwork, SubscribeQuoteReceivesData) {
    if (!network_enabled()) {
        GTEST_SKIP() << "set TQSDK_RUN_NETWORK=1 to run network tests";
    }

    asio::io_context ctx;
    auto             md = std::make_unique<TqMdGateway>(ctx.get_executor());
    // td 传 nullptr：只验证行情链路。
    TqApi api(ctx.get_executor(), std::move(md), nullptr);
    api.start();

    bool   got_update = false;
    double last_price = 0;
    double turnover   = 0;

    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            api.get_quote("SHFE.rb2610"); // 触发订阅（get_quote 内部会 subscribe）
            // 最多等待若干次更新，直到拿到 finite last_price。
            for (int i = 0; i < 50; ++i) {
                co_await api.wait_update();
                auto& q = api.get_quote("SHFE.rb2610");
                if (std::isfinite(q.last_price) && q.last_price > 0) {
                    last_price = q.last_price;
                    turnover   = q.turnover;
                    got_update = true;
                    break;
                }
            }
            co_return;
        },
        [](std::exception_ptr) {});

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!got_update && std::chrono::steady_clock::now() < deadline) {
        test::pump(ctx, 50);
    }

    api.close();
    test::pump(ctx, 50);

    EXPECT_TRUE(got_update) << "did not receive a finite last_price within timeout";
    if (got_update) {
        EXPECT_GT(last_price, 0.0);
        EXPECT_TRUE(std::isfinite(turnover));
    }
}

// 真实连接 -> get_kline_serial 订阅 -> 收到 K 线 -> bars 非空且字段有效。
TEST(TqMdNetwork, KlineSerialReceivesBars) {
    if (!network_enabled()) {
        GTEST_SKIP() << "set TQSDK_RUN_NETWORK=1 to run network tests";
    }

    asio::io_context ctx;
    auto             md = std::make_unique<TqMdGateway>(ctx.get_executor());
    TqApi            api(ctx.get_executor(), std::move(md), nullptr);
    api.start();

    bool   got_bars   = false;
    size_t nbars      = 0;
    double last_close = 0;

    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            // 语义对齐 TqSdk：get_kline_serial 创建序列并发起订阅。
            KlineStream* k = api.get_kline_serial("SHFE.rb2610", 60'000'000'000LL, 20);
            for (int i = 0; i < 50; ++i) {
                co_await api.wait_update();
                k = api.get_kline("SHFE.rb2610", 60'000'000'000LL);
                if (k != nullptr && !k->bars.empty()) {
                    nbars      = k->bars.size();
                    last_close = k->bars.back().close;
                    got_bars   = std::isfinite(last_close) && last_close > 0;
                    if (got_bars) {
                        break;
                    }
                }
            }
            co_return;
        },
        [](std::exception_ptr) {});

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!got_bars && std::chrono::steady_clock::now() < deadline) {
        test::pump(ctx, 50);
    }

    api.close();
    test::pump(ctx, 50);

    EXPECT_TRUE(got_bars) << "did not receive kline bars within timeout";
    EXPECT_GT(nbars, 0u);
    EXPECT_GT(last_close, 0.0);
}
