#include "tqsdk/pch.h"

#include "tqsdk/gateway/mock/mock_md_gateway.h"
#include "tqsdk/gateway/mock/mock_td_gateway.h"
#include "tqsdk/tqapi.h"

#include "support/test_helpers.h"

using namespace tqsdk;
using json = nlohmann::json;

namespace {

// 手动驱动：spawn 协程后由测试自己 pump / 注入，最后检查是否完成。
// 相比 test::run_coro（一口气跑到结束），这里需要在协程"挂起中"注入数据。
//
// 注意：协程体内严禁使用 ASSERT_* / FAIL_*（它们会展开出 `return;`，
// 在协程里非法）。所有断言一律放到协程外，用捕获变量承载结果。
struct Fixture {
    asio::io_context       ctx;
    MockMdGateway*         md = nullptr;
    MockTdGateway*         td = nullptr;
    std::unique_ptr<TqApi> api;

    bool               done = false;
    std::exception_ptr eptr;
    // 注意：协程帧会引用传入闭包的捕获；闭包必须存活到协程结束，
    // 所以把 std::function 存成成员，避免 spawn 返回后闭包悬垂。
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
        // 必须先让后台 md_loop/td_loop 退出，再析构网关(Channel)。
        // 否则挂起在 Channel::async_receive 里的协程会在 io_context 析构时
        // 触碰已被释放的 waiter 列表 -> use-after-free。
        if (api) {
            api->close();
            // 驱动 ctx 直到所有已投递任务(被唤醒的协程)跑完。
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

} // namespace

// 行情注入 -> wait_update -> get_quote 读到 -> is_changing 正确
TEST(MockE2E, WaitUpdateThenReadQuote) {
    Fixture f;

    bool     got_ok   = false;
    double   last     = 0;
    uint64_t vol      = 0;
    bool     changing = false;

    f.spawn([&]() -> asio::awaitable<void> {
        got_ok = co_await f.api->wait_update();

        auto& q  = f.api->get_quote("SHFE.rb2610");
        last     = q.last_price;
        vol      = q.volume;
        changing = f.api->is_changing(q, (uint64_t)QuoteField::last_price);
        co_return;
    });

    f.pump(20); // 让协程挂起在 wait_update
    EXPECT_FALSE(f.done);

    f.md->inject(quote_diff("SHFE.rb2610", {{"last_price", 3800.0}, {"volume", 42}}));
    f.finish_and_check();

    EXPECT_TRUE(got_ok);
    EXPECT_EQ(last, 3800.0);
    EXPECT_EQ(vol, 42u);
    EXPECT_TRUE(changing);
}

// 发单 -> 本地占位单立即可见 -> 终态 diff 延迟到达 -> wait_update 后 is_dead
TEST(MockE2E, InsertOrderAsyncThenSettle) {
    Fixture f;
    f.td->reply_delay_ms = 5;

    std::string rid;
    bool        non_null                = false;
    bool        dead_right_after_insert = true;
    bool        alive_before_update     = false;
    bool        dead_after_update       = false;
    uint64_t    volume_orign            = 0;

    f.spawn([&]() -> asio::awaitable<void> {
        InsertOrderReq req;
        req.instrument_id = "SHFE.rb2610";
        req.direction     = Direction::BUY;
        req.offset        = Offset::OPEN;
        req.price_type    = PriceType::LIMIT;
        req.limit_price   = 3800.0;
        req.volume        = 1;

        Order* o = co_await f.api->insert_order(req);
        if (!o) {
            co_return;
        }
        non_null     = true;
        rid          = o->order_id;
        volume_orign = o->volume_orign;

        // 发单立即返回，终态 diff 还没到 -> 仍 alive（核心断言）
        dead_right_after_insert = o->is_dead;
        alive_before_update     = !o->is_dead;

        bool ok = co_await f.api->wait_update();
        (void)ok;

        auto* done = f.api->get_order(rid);
        if (done) {
            dead_after_update = done->is_dead;
        }
        co_return;
    });

    f.finish_and_check();

    EXPECT_TRUE(non_null);
    EXPECT_FALSE(rid.empty());
    EXPECT_EQ(volume_orign, 1u);
    EXPECT_FALSE(dead_right_after_insert);
    EXPECT_TRUE(alive_before_update);
    EXPECT_TRUE(dead_after_update);
}

// 柜台拒绝 -> 占位单立即变为错误终态，无需等 diff
TEST(MockE2E, InsertOrderRejected) {
    Fixture f;
    f.td->reject_next = true;

    bool dead = false;
    bool err  = false;

    f.spawn([&]() -> asio::awaitable<void> {
        InsertOrderReq req;
        req.instrument_id = "SHFE.rb2610";
        req.volume        = 1;
        req.limit_price   = 3800.0;

        Order* o = co_await f.api->insert_order(req);
        if (o) {
            dead = o->is_dead;
            err  = o->is_error;
        }
        co_return;
    });

    f.finish_and_check();
    EXPECT_TRUE(dead);
    EXPECT_TRUE(err);
}

// close 后挂起的 wait_update 被唤醒并返回 false
TEST(MockE2E, CloseWakesWaitUpdate) {
    Fixture f;
    bool    got = true;

    f.spawn([&]() -> asio::awaitable<void> {
        got = co_await f.api->wait_update(); // 无数据 -> 挂起；close 后返回 false
        co_return;
    });

    f.pump(20);
    EXPECT_FALSE(f.done);

    f.api->close();
    f.finish_and_check();
    EXPECT_FALSE(got);
}

// wait_update 返回后位仍可读（本轮变化），下一次 wait_update 进入时清位
TEST(MockE2E, IsChangingVisibleUntilNextWait) {
    Fixture  f;
    bool     second_returned     = false;
    uint64_t changed_after_first = 0;

    f.spawn([&]() -> asio::awaitable<void> {
        co_await f.api->wait_update();
        auto& q             = f.api->get_quote("SHFE.rb2610");
        changed_after_first = q.changed_fields; // 本轮变化：应非 0

        // 再次进入 wait_update：本轮无新数据 -> 挂起
        co_await f.api->wait_update();
        second_returned = true;
        co_return;
    });

    f.pump(20);
    f.md->inject(quote_diff("SHFE.rb2610", {{"last_price", 3800.0}}));
    f.pump(30);

    // 第一次返回后位的可见性
    EXPECT_NE(changed_after_first, 0u);
    // 第二次已挂起（未注入新数据）
    EXPECT_FALSE(second_returned);

    f.api->close();
    f.finish_and_check();
}

// quote diff -> TickSeries 固化快照；重复相同快照不会重复追加。
TEST(MockE2E, TickSeriesAppendsQuoteSnapshots) {
    Fixture f;
    auto*   ticks = f.api->get_tick_serial("SHFE.rb2610", 3);
    ASSERT_NE(ticks, nullptr);
    f.md->inject(quote_diff("SHFE.rb2610",
                            {{"datetime", "2026-11-09 21:00:00.000000"},
                             {"last_price", 3800.0},
                             {"volume", 42},
                             {"bid_price1", 3799.0},
                             {"ask_price1", 3801.0}}));
    f.pump(30);
    ASSERT_EQ(ticks->data.size(), 1u);
    EXPECT_EQ(ticks->data.back()->last_price, 3800.0);
    EXPECT_EQ(ticks->data.back()->bid_price[0], 3799.0);
    EXPECT_EQ(ticks->data.back()->ask_price[0], 3801.0);

    f.md->inject(quote_diff("SHFE.rb2610",
                            {{"datetime", "2026-11-09 21:00:00.000000"},
                             {"last_price", 3800.0},
                             {"volume", 42},
                             {"bid_price1", 3799.0},
                             {"ask_price1", 3801.0}}));
    f.pump(30);
    EXPECT_EQ(ticks->data.size(), 1u);

    f.md->inject(quote_diff("SHFE.rb2610",
                            {{"datetime", "2026-11-09 21:00:01.000000"},
                             {"last_price", 3802.0},
                             {"volume", 43},
                             {"bid_price1", 3801.0},
                             {"ask_price1", 3803.0}}));
    f.pump(30);
    ASSERT_EQ(ticks->data.size(), 2u);
    EXPECT_EQ(ticks->data.back()->last_price, 3802.0);
}

// tick sink 只在真正新增快照时触发，重复 quote 不重复持久化。
TEST(MockE2E, TickSinkReceivesOnlyAppendedSnapshots) {
    Fixture f;
    f.api->get_tick_serial("SHFE.rb2610", 3);
    std::vector<Tick> seen;
    f.api->set_tick_sink([&](const Tick& tick) { seen.push_back(tick); });

    auto quote = [](std::string datetime, double last, uint64_t volume) {
        return quote_diff("SHFE.rb2610",
                          {{"datetime", std::move(datetime)},
                           {"last_price", last},
                           {"volume", volume},
                           {"bid_price1", last - 1},
                           {"ask_price1", last + 1}});
    };
    f.md->inject(quote("2026-11-09 21:00:00.000000", 3800, 42));
    f.pump(30);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.back().last_price, 3800);
    EXPECT_EQ(seen.back().bid_price[0], 3799);

    f.md->inject(quote("2026-11-09 21:00:00.000000", 3800, 42));
    f.pump(30);
    EXPECT_EQ(seen.size(), 1u);

    f.md->inject(quote("2026-11-09 21:00:01.000000", 3802, 43));
    f.pump(30);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen.back().ask_price[0], 3803);
}
