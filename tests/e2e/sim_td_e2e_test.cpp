#include "tqsdk/pch.h"

#include "tqsdk/gateway/sim/sim_td_gateway.h"
#include "tqsdk/tqapi.h"

#include "support/test_helpers.h"

using namespace tqsdk;
using json = nlohmann::json;

namespace {

// 驱动 SimTdGateway + TqApi 的测试夹具。
//   - sim : 直接持有 sim 指针调 on_tick 驱动撮合（扮演"交易所/市场时钟"）
//   - api : 走 TqApi 全链路（wait_update + get_order/get_account/get_position）做断言
struct SimFixture {
    asio::io_context       ctx;
    SimTdGateway*          sim = nullptr;
    std::unique_ptr<TqApi> api;

    bool                                   done = false;
    std::exception_ptr                     eptr;
    std::function<asio::awaitable<void>()> body_;

    static ContractSpec default_spec() {
        ContractSpec s;
        s.volume_multiple    = 10.0; // 每点 10 元
        s.price_tick         = 1.0;
        s.margin_rate        = 0.1; // 10% 保证金
        s.commission_rate    = 0.0;
        s.commission_per_lot = 2.0; // 每手 2 元
        return s;
    }

    explicit SimFixture(double balance = 1'000'000.0) {
        auto td_owner = std::make_unique<SimTdGateway>(ctx.get_executor(), balance);
        sim           = td_owner.get();
        sim->set_contract_spec("SHFE.rb2610", default_spec());
        api = std::make_unique<TqApi>(ctx.get_executor(), nullptr, std::move(td_owner));
        api->start();
    }

    ~SimFixture() {
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
        if (eptr)
            std::rethrow_exception(eptr);
        EXPECT_TRUE(done) << "coroutine did not complete";
    }
};

InsertOrderReq make_limit(Direction dir, Offset off, double price, uint64_t vol) {
    InsertOrderReq r;
    r.instrument_id = "SHFE.rb2610";
    r.direction     = dir;
    r.offset        = off;
    r.price_type    = PriceType::LIMIT;
    r.limit_price   = price;
    r.volume        = vol;
    return r;
}

InsertOrderReq make_market(Direction dir, Offset off, uint64_t vol) {
    InsertOrderReq r;
    r.instrument_id = "SHFE.rb2610";
    r.direction     = dir;
    r.offset        = off;
    r.price_type    = PriceType::ANY;
    r.volume        = vol;
    return r;
}

Position* get_position(TqApi& api, const std::string& sym) {
    return api.data().positions.find(sym);
}

} // namespace

// 市价单在已有报价时立即成交：order 终态 + trade + position + account 全链路
TEST(SimTdE2E, MarketOrderFillsImmediately) {
    SimFixture f;
    f.sim->on_tick("SHFE.rb2610", 3800.0); // 先有报价

    std::string rid;
    f.spawn([&]() -> asio::awaitable<void> {
        Order* o = co_await f.api->insert_order(make_market(Direction::BUY, Offset::OPEN, 2));
        rid      = o->order_id;
        co_await f.api->wait_update();
        co_return;
    });

    f.finish_and_check();

    auto* o = f.api->get_order(rid);
    ASSERT_NE(o, nullptr);
    EXPECT_TRUE(o->is_dead);
    EXPECT_EQ(o->volume_left, 0u);
    EXPECT_EQ(o->trade_price, 3800.0);

    auto* p = get_position(*f.api, "SHFE.rb2610");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->volume_long, 2u);

    auto& acc = f.api->get_account();
    // 手续费 = 每手 2 元 * 2 手 = 4 元
    EXPECT_EQ(acc.commission, 4.0);
    // 保证金 = 3800 * 10 * 2 * 0.1 = 7600
    EXPECT_EQ(acc.margin, 7600.0);
    EXPECT_EQ(acc.available, 1000000.0 - 4.0 - 7600.0);

    EXPECT_EQ(f.api->data().trades.size(), 1u);
}

// 限价买单未穿越 -> 挂单；on_tick 穿越 -> 成交
TEST(SimTdE2E, LimitBuyRestsThenFillsOnTick) {
    SimFixture f;

    std::string rid;
    bool        dead_before = true;
    uint64_t    left_before = 0;
    bool        dead_after  = false;
    double      trade_px    = 0;

    f.spawn([&]() -> asio::awaitable<void> {
        // 无报价时先挂限价买单 3790；insert 会产生"已报(alive)"diff
        Order* o =
            co_await f.api->insert_order(make_limit(Direction::BUY, Offset::OPEN, 3790.0, 1));
        rid = o->order_id;
        co_await f.api->wait_update(); // 消化 alive 回执

        // 报价 3800 高于限价，不成交（无 diff，不能 wait_update）
        f.sim->on_tick("SHFE.rb2610", 3800.0);

        auto* cur   = f.api->get_order(rid);
        dead_before = cur->is_dead;
        left_before = cur->volume_left;

        // 报价跌到 3790，穿越成交
        f.sim->on_tick("SHFE.rb2610", 3790.0);
        co_await f.api->wait_update();

        auto* done = f.api->get_order(rid);
        dead_after = done->is_dead;
        trade_px   = done->trade_price;
        co_return;
    });

    f.finish_and_check();

    EXPECT_FALSE(dead_before) << "should still be alive before crossing";
    EXPECT_EQ(left_before, 1u);
    EXPECT_TRUE(dead_after);
    EXPECT_EQ(trade_px, 3790.0);
}

// 限价卖单：报价 >= 限价才成交
TEST(SimTdE2E, LimitSellFillsWhenPriceRises) {
    SimFixture f;

    f.spawn([&]() -> asio::awaitable<void> {
        f.sim->on_tick("SHFE.rb2610", 3800.0);
        Order* o =
            co_await f.api->insert_order(make_limit(Direction::SELL, Offset::OPEN, 3810.0, 1));
        co_await f.api->wait_update();
        EXPECT_FALSE(f.api->get_order(o->order_id)->is_dead); // 3800 < 3810 未成

        f.sim->on_tick("SHFE.rb2610", 3810.0); // 到价
        co_await f.api->wait_update();
        auto* cur = f.api->get_order(o->order_id);
        EXPECT_TRUE(cur->is_dead);
        EXPECT_EQ(cur->trade_price, 3810.0);
        if (auto* p = get_position(*f.api, "SHFE.rb2610"))
            EXPECT_EQ(p->volume_short, 1u);
        co_return;
    });

    f.finish_and_check();
}

// 多笔限价单随行情逐级成交
TEST(SimTdE2E, MultipleTicksAccumulate) {
    SimFixture f;

    f.spawn([&]() -> asio::awaitable<void> {
        f.sim->on_tick("SHFE.rb2610", 3800.0);
        // 两笔限价单，价都低于当前价 -> 都挂起
        Order* a =
            co_await f.api->insert_order(make_limit(Direction::BUY, Offset::OPEN, 3790.0, 1));
        Order* b =
            co_await f.api->insert_order(make_limit(Direction::BUY, Offset::OPEN, 3780.0, 1));
        co_await f.api->wait_update();
        EXPECT_FALSE(f.api->get_order(a->order_id)->is_dead);

        f.sim->on_tick("SHFE.rb2610", 3790.0); // 只成交 a
        co_await f.api->wait_update();
        EXPECT_TRUE(f.api->get_order(a->order_id)->is_dead);
        EXPECT_EQ(f.api->get_order(a->order_id)->trade_price, 3790.0);
        EXPECT_FALSE(f.api->get_order(b->order_id)->is_dead);

        f.sim->on_tick("SHFE.rb2610", 3780.0); // 成交 b
        co_await f.api->wait_update();
        EXPECT_TRUE(f.api->get_order(b->order_id)->is_dead);
        EXPECT_EQ(f.api->get_order(b->order_id)->trade_price, 3780.0);
        co_return;
    });

    f.finish_and_check();
    EXPECT_EQ(f.api->data().trades.size(), 2u);
}

// 撤单：挂单后撤单 -> 终态 + 资金释放
TEST(SimTdE2E, CancelReleasesFrozen) {
    SimFixture f;

    std::string rid;
    f.spawn([&]() -> asio::awaitable<void> {
        Order* o =
            co_await f.api->insert_order(make_limit(Direction::BUY, Offset::OPEN, 3800.0, 1));
        rid = o->order_id;
        co_await f.api->wait_update();
        // 挂单冻结 = 3800*10*1*0.1 = 3800
        EXPECT_EQ(f.api->get_account().frozen_margin, 3800.0);

        CancelOrderReq c;
        c.order_id = rid;
        co_await f.api->cancel_order(c);
        co_await f.api->wait_update();
        co_return;
    });

    f.finish_and_check();
    EXPECT_EQ(f.api->get_account().frozen_margin, 0.0);
    EXPECT_TRUE(f.api->get_order(rid)->is_dead);
    EXPECT_EQ(f.api->get_order(rid)->last_msg, "已撤销");
}

// 平仓：开仓后平仓，持仓归零、保证金释放、产生平仓盈亏
TEST(SimTdE2E, OpenThenClose) {
    SimFixture f;

    f.spawn([&]() -> asio::awaitable<void> {
        f.sim->on_tick("SHFE.rb2610", 3800.0);
        co_await f.api->insert_order(make_market(Direction::BUY, Offset::OPEN, 1));
        co_await f.api->wait_update();
        auto* p = get_position(*f.api, "SHFE.rb2610");
        EXPECT_TRUE(p != nullptr);
        if (p)
            EXPECT_EQ(p->volume_long, 1u);

        f.sim->on_tick("SHFE.rb2610", 3850.0); // 涨到 3850
        co_await f.api->insert_order(make_market(Direction::SELL, Offset::CLOSE, 1));
        co_await f.api->wait_update();
        EXPECT_EQ(get_position(*f.api, "SHFE.rb2610")->volume_long, 0u);
        // 平仓盈亏 = (3850-3800)*10*1 = 500
        EXPECT_EQ(f.api->get_account().close_profit, 500.0);
        EXPECT_EQ(f.api->get_account().margin, 0.0);
        co_return;
    });

    f.finish_and_check();
}

// 拒单：未知合约
TEST(SimTdE2E, RejectUnknownInstrument) {
    SimFixture f;

    f.spawn([&]() -> asio::awaitable<void> {
        InsertOrderReq r;
        r.instrument_id = "UNKNOWN.xx";
        r.volume        = 1;
        r.price_type    = PriceType::LIMIT;
        r.limit_price   = 100.0;
        Order*      o   = co_await f.api->insert_order(r);
        std::string rid = o->order_id;
        co_await f.api->wait_update();
        auto* cur = f.api->get_order(rid);
        EXPECT_TRUE(cur->is_error);
        EXPECT_TRUE(cur->is_dead);
        co_return;
    });

    f.finish_and_check();
}

// 拒单：手数为 0
TEST(SimTdE2E, RejectZeroVolume) {
    SimFixture f;

    f.spawn([&]() -> asio::awaitable<void> {
        Order* o =
            co_await f.api->insert_order(make_limit(Direction::BUY, Offset::OPEN, 3800.0, 0));
        std::string rid = o->order_id;
        co_await f.api->wait_update();
        EXPECT_TRUE(f.api->get_order(rid)->is_error);
        co_return;
    });

    f.finish_and_check();
}

// FOK/FAK：未穿越时 IOC 语义应直接终态，不留在模拟挂单簿。
TEST(SimTdE2E, FokNotCrossingBecomesFinished) {
    SimFixture f;

    f.spawn([&]() -> asio::awaitable<void> {
        f.sim->on_tick("SHFE.rb2610", 3800.0);
        auto req              = make_limit(Direction::BUY, Offset::OPEN, 3700.0, 2);
        req.advanced          = AdvancedOrder::FOK;
        req.time_cond         = TimeCondition::IOC;
        req.volume_cond       = VolumeCondition::ALL;
        Order*            o   = co_await f.api->insert_order(req);
        const std::string rid = o->order_id;
        co_await f.api->wait_update();
        auto* cur = f.api->get_order(rid);
        EXPECT_NE(cur, nullptr);
        if (cur == nullptr)
            co_return;
        EXPECT_TRUE(cur->is_dead);
        EXPECT_EQ(cur->volume_left, 2u);
        EXPECT_EQ(cur->advanced, AdvancedOrder::FOK);
        EXPECT_EQ(cur->time_cond, TimeCondition::IOC);
        EXPECT_EQ(cur->volume_cond, VolumeCondition::ALL);
        EXPECT_NE(cur->last_msg.find("IOC"), std::string::npos);
        EXPECT_EQ(f.api->data().positions.size(), 0u);
        co_return;
    });

    f.finish_and_check();
}

// 市价单无报价 -> 拒单
TEST(SimTdE2E, RejectMarketOrderWithoutQuote) {
    SimFixture f;

    f.spawn([&]() -> asio::awaitable<void> {
        Order*      o = co_await f.api->insert_order(make_market(Direction::BUY, Offset::OPEN, 1));
        std::string rid = o->order_id;
        co_await f.api->wait_update();
        auto* cur = f.api->get_order(rid);
        EXPECT_TRUE(cur->is_error);
        EXPECT_TRUE(cur->is_dead);
        co_return;
    });

    f.finish_and_check();
}

// 事务性：一次 on_tick 触发多笔成交 -> 一次 wait_update 后全部可见
TEST(SimTdE2E, SingleUpdateSeesAllFills) {
    SimFixture f;

    std::string a_rid, b_rid;
    f.spawn([&]() -> asio::awaitable<void> {
        f.sim->on_tick("SHFE.rb2610", 3800.0);
        Order* a =
            co_await f.api->insert_order(make_limit(Direction::BUY, Offset::OPEN, 3791.0, 1));
        Order* b =
            co_await f.api->insert_order(make_limit(Direction::BUY, Offset::OPEN, 3792.0, 1));
        a_rid = a->order_id;
        b_rid = b->order_id;
        co_await f.api->wait_update(); // 消化挂单

        // 一次行情穿越两笔
        f.sim->on_tick("SHFE.rb2610", 3790.0);
        co_await f.api->wait_update();

        EXPECT_TRUE(f.api->get_order(a_rid)->is_dead);
        EXPECT_TRUE(f.api->get_order(b_rid)->is_dead);
        EXPECT_EQ(f.api->data().trades.size(), 2u);
        co_return;
    });

    f.finish_and_check();
}

// snapshot() 提供完整交易截面，可被 DiffMergeEngine 重建成等价 DataManager。
TEST(SimTdE2E, SnapshotContainsFullTradingState) {
    SimFixture f;

    f.spawn([&]() -> asio::awaitable<void> {
        f.sim->on_tick("SHFE.rb2610", 3800.0);
        co_await f.api->insert_order(make_market(Direction::BUY, Offset::OPEN, 2));
        co_await f.api->wait_update();
        co_return;
    });
    f.finish_and_check();

    const json snap = f.sim->snapshot();
    ASSERT_TRUE(snap.contains("trade"));
    const auto& user = snap["trade"][""]["user"];
    ASSERT_TRUE(user.contains("orders"));
    ASSERT_TRUE(user.contains("trades"));
    ASSERT_TRUE(user.contains("positions"));
    ASSERT_TRUE(user.contains("accounts"));
    EXPECT_EQ(user["orders"].size(), 1u);
    EXPECT_EQ(user["trades"].size(), 1u);
    EXPECT_EQ(user["positions"].size(), 1u);

    DataManager     dm;
    DiffMergeEngine eng(dm);
    eng.apply(snap);
    EXPECT_EQ(dm.orders.size(), 1u);
    EXPECT_EQ(dm.trades.size(), 1u);
    EXPECT_EQ(dm.positions.size(), 1u);
    EXPECT_DOUBLE_EQ(dm.account.balance, 999996.0);
    EXPECT_DOUBLE_EQ(dm.account.margin, 7600.0);
}
