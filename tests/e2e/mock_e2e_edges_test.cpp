#include "tqsdk/pch.h"

#include "tqsdk/gateway/mock/mock_md_gateway.h"
#include "tqsdk/gateway/mock/mock_td_gateway.h"
#include "tqsdk/tqapi.h"

#include "support/test_helpers.h"

using namespace tqsdk;
using json = nlohmann::json;

namespace {

// 与 mock_e2e_test.cpp 中的 Fixture 同构：手动驱动 + 安全收尾。
struct Fixture {
    asio::io_context       ctx;
    MockMdGateway*         md = nullptr;
    MockTdGateway*         td = nullptr;
    std::unique_ptr<TqApi> api;

    bool                                   done = false;
    std::exception_ptr                     eptr;
    std::function<asio::awaitable<void>()> body_;

    Fixture() {
        auto md_owner = std::make_unique<MockMdGateway>(ctx.get_executor());
        auto td_owner = std::make_unique<MockTdGateway>(ctx.get_executor());
        md            = md_owner.get();
        td            = td_owner.get();
        api = std::make_unique<TqApi>(ctx.get_executor(), std::move(md_owner), std::move(td_owner));
        api->start();
    }

    ~Fixture() {
        if (api) {
            api->close();
            ctx.run();
        }
    }

    void spawn(std::function<asio::awaitable<void>()> body) {
        body_ = std::move(body);
        asio::co_spawn(ctx, body_(), [this](std::exception_ptr ep) {
            eptr = ep;
            done = true;
        });
    }

    void pump(int ms = 20) { test::pump(ctx, ms); }

    void finish_and_check(int ms = 300) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!done && std::chrono::steady_clock::now() < deadline) {
            pump(20);
        }
        if (eptr) {
            std::rethrow_exception(eptr);
        }
        EXPECT_TRUE(done) << "coroutine did not complete";
    }
};

nlohmann::json quote_diff(std::string_view sym, json fields) {
    return json{{"quotes", {{std::string(sym), std::move(fields)}}}};
}

// 构造一条仅含 accounts 的 trade diff。
nlohmann::json account_diff(json accounts) {
    return json{{"trade", {{"", {{"user", {{"accounts", std::move(accounts)}}}}}}}};
}

} // namespace

// 账户端到端：td 推 accounts -> wait_update -> get_account 读到 + is_changing(account)
TEST(MockE2EEdges, AccountUpdate) {
    Fixture f;

    bool   got_ok   = false;
    double balance  = 0;
    double avail    = 0;
    bool   changing = false;

    f.spawn([&]() -> asio::awaitable<void> {
        got_ok   = co_await f.api->wait_update();
        auto& a  = f.api->get_account();
        balance  = a.balance;
        avail    = a.available;
        changing = f.api->is_changing(a, (uint64_t)AccountField::balance);
        co_return;
    });

    f.pump(20);
    EXPECT_FALSE(f.done);

    f.td->inject_diff(account_diff({{"CNY", {{"balance", 1000000.0}, {"available", 800000.0}}}}));
    f.finish_and_check();

    EXPECT_TRUE(got_ok);
    EXPECT_EQ(balance, 1000000.0);
    EXPECT_EQ(avail, 800000.0);
    EXPECT_TRUE(changing);
}

// K 线端到端：md 推 klines -> wait_update -> get_kline 可见
TEST(MockE2EEdges, KlineUpdate) {
    Fixture f;

    bool        got_ok   = false;
    std::size_t nbars    = 0;
    double      close    = 0;
    bool        appended = false;

    f.spawn([&]() -> asio::awaitable<void> {
        got_ok  = co_await f.api->wait_update();
        auto* k = f.api->get_kline("SHFE.rb2610", 180000000000LL);
        if (k) {
            nbars    = k->bars.size();
            close    = k->bars.empty() ? -1 : k->bars.back().close;
            appended = (k->changed_fields & KlineChangeDetail::Appended) != 0;
        }
        co_return;
    });

    f.pump(20);
    f.md->inject(json::parse(R"({"klines":{"SHFE.rb2610":{"180000000000":
        [{"datetime":192837300000000,"open":3800,"high":3810,"low":3790,"close":3805}]}}})"));
    f.finish_and_check();

    EXPECT_TRUE(got_ok);
    EXPECT_EQ(nbars, 1u);
    EXPECT_EQ(close, 3805.0);
    EXPECT_TRUE(appended);
}

// 一次推送同时含 quotes 与 trade：一次 wait_update 后两者都更新
TEST(MockE2EEdges, MixedDiffSingleUpdate) {
    Fixture f;

    bool   got_ok   = false;
    double last     = 0;
    bool   order_ok = false;

    f.spawn([&]() -> asio::awaitable<void> {
        got_ok   = co_await f.api->wait_update();
        auto& q  = f.api->get_quote("SHFE.rb2610");
        last     = q.last_price;
        auto* o  = f.api->get_order("oid_x");
        order_ok = (o != nullptr) && o->is_dead;
        co_return;
    });

    f.pump(20);
    f.md->inject(json::parse(R"({
        "quotes":{"SHFE.rb2610":{"last_price":3800.0}},
        "trade":{"":{"user":{"orders":{"oid_x":{
            "instrument_id":"SHFE.rb2610","status":"FINISHED","is_dead":true
        }}}}}
    })"));
    f.finish_and_check();

    EXPECT_TRUE(got_ok);
    EXPECT_EQ(last, 3800.0);
    EXPECT_TRUE(order_ok);
}

// 多轮更新：每轮 wait_update 都能读到对应变化
TEST(MockE2EEdges, SequentialUpdates) {
    Fixture f;

    std::vector<double> seen;
    int                 rounds = 0;

    f.spawn([&]() -> asio::awaitable<void> {
        for (int i = 0; i < 3; ++i) {
            co_await f.api->wait_update();
            seen.push_back(f.api->get_quote("SHFE.rb2610").last_price);
            ++rounds;
        }
        co_return;
    });

    f.pump(20);
    f.md->inject(quote_diff("SHFE.rb2610", {{"last_price", 3800.0}}));
    f.pump(30);
    f.md->inject(quote_diff("SHFE.rb2610", {{"last_price", 3801.0}}));
    f.pump(30);
    f.md->inject(quote_diff("SHFE.rb2610", {{"last_price", 3802.0}}));
    f.finish_and_check();

    EXPECT_EQ(rounds, 3);
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], 3800.0);
    EXPECT_EQ(seen[1], 3801.0);
    EXPECT_EQ(seen[2], 3802.0);
}

// get_quote 幂等：同标的多取多次同引用；get_order 不存在返回 nullptr
TEST(MockE2EEdges, AccessorIdempotent) {
    Fixture f;

    f.spawn([&]() -> asio::awaitable<void> {
        auto& q1 = f.api->get_quote("SHFE.rb2610");
        auto& q2 = f.api->get_quote("SHFE.rb2610");
        EXPECT_EQ(&q1, &q2);
        EXPECT_EQ(f.api->data().quotes.size(), 1u);

        EXPECT_EQ(f.api->get_order("nope"), nullptr);
        EXPECT_EQ(f.api->get_kline("SHFE.rb2610", 12345LL), nullptr);
        co_return;
    });

    f.finish_and_check();
}

// md 推 Error 事件：后台 loop 记录日志但不崩，后续数据仍能正常处理
TEST(MockE2EEdges, MdErrorEventDoesNotCrash) {
    Fixture f;

    bool   got_ok = false;
    double last   = 0;

    f.spawn([&]() -> asio::awaitable<void> {
        got_ok = co_await f.api->wait_update();
        last   = f.api->get_quote("SHFE.rb2610").last_price;
        co_return;
    });

    f.pump(20);
    f.md->inject_error("mock md failure"); // 不应触发 wait_update
    f.pump(20);
    EXPECT_FALSE(f.done); // Error 不唤醒 wait_update

    f.md->inject(quote_diff("SHFE.rb2610", {{"last_price", 3800.0}}));
    f.finish_and_check();

    EXPECT_TRUE(got_ok);
    EXPECT_EQ(last, 3800.0);
}

// cancel_order 有网关时回 ack
TEST(MockE2EEdges, CancelOrderAck) {
    Fixture f;

    bool        success = false;
    std::string oid;

    f.spawn([&]() -> asio::awaitable<void> {
        CancelOrderReq req;
        req.order_id = "rid_manual";
        auto ack     = co_await f.api->cancel_order(req);
        success      = ack.success;
        oid          = ack.order_id;
        co_return;
    });

    f.finish_and_check();
    EXPECT_TRUE(success);
    EXPECT_EQ(oid, "rid_manual");
}

// get_kline_serial 语义：调用即创建本地流（非空），重复调用复用同一流；
// 随后注入 klines 能在该流上读到 bars。默认 mock 网关不覆写 subscribe_kline，
// 走基类空实现，不影响本地流创建。
TEST(MockE2EEdges, GetKlineSerialCreatesAndReuses) {
    Fixture f;

    KlineStream* first  = nullptr;
    KlineStream* second = nullptr;
    uint64_t     cap    = 0;
    std::size_t  nbars  = 0;

    f.spawn([&]() -> asio::awaitable<void> {
        // 创建即返回非空流，重复调用复用同一流。
        first  = f.api->get_kline_serial("SHFE.rb2610", 180000000000LL, 20);
        second = f.api->get_kline_serial("SHFE.rb2610", 180000000000LL, 20);
        if (first) {
            cap = first->capacity;
        }
        // 等注入的 klines 合并后读到 bars。
        co_await f.api->wait_update();
        auto* k = f.api->get_kline("SHFE.rb2610", 180000000000LL);
        if (k) {
            nbars = k->bars.size();
        }
        co_return;
    });

    f.pump(20);
    f.md->inject(json::parse(R"({"klines":{"SHFE.rb2610":{"180000000000":
        [{"datetime":192837300000000,"open":3800,"high":3810,"low":3790,"close":3805}]}}})"));
    f.finish_and_check();

    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, second); // 复用同一流
    EXPECT_EQ(cap, 20u);      // capacity 取 view_width
    EXPECT_EQ(nbars, 1u);
}

// ---- 无网关分支：只给 md，不给 td ----

// insert_order 在没有 td 时立即错误终态（不需要 wait_update）
TEST(MockE2EEdges, InsertOrderWithoutTd) {
    asio::io_context ctx;
    auto             md_owner = std::make_unique<MockMdGateway>(ctx.get_executor());
    TqApi            api(ctx.get_executor(), std::move(md_owner), nullptr);
    api.start();

    bool dead = false;
    bool err  = false;
    bool done = false;

    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            InsertOrderReq req;
            req.instrument_id = "SHFE.rb2610";
            req.volume        = 1;
            req.limit_price   = 3800.0;
            Order* o          = co_await api.insert_order(req);
            if (o) {
                dead = o->is_dead;
                err  = o->is_error;
            }
            done = true;
            co_return;
        },
        [](std::exception_ptr) {});

    test::pump(ctx, 50);
    EXPECT_TRUE(done);
    EXPECT_TRUE(dead);
    EXPECT_TRUE(err);

    api.close();
    ctx.run();
}

// cancel_order 无 td 时回失败 ack
TEST(MockE2EEdges, CancelOrderWithoutTd) {
    asio::io_context ctx;
    auto             md_owner = std::make_unique<MockMdGateway>(ctx.get_executor());
    TqApi            api(ctx.get_executor(), std::move(md_owner), nullptr);
    api.start();

    bool success = true;
    bool done    = false;

    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            CancelOrderReq req;
            req.order_id = "rid_x";
            auto ack     = co_await api.cancel_order(req);
            success      = ack.success;
            done         = true;
            co_return;
        },
        [](std::exception_ptr) {});

    test::pump(ctx, 50);
    EXPECT_TRUE(done);
    EXPECT_FALSE(success);

    api.close();
    ctx.run();
}
