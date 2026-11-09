#include "tqsdk/pch.h"

#include "tqsdk/gateway/sim/sim_td_gateway.h"
#include "tqsdk/tqapi.h"
#include "tqsdk/trading/order_tasks.h"
#include "tqsdk/trading/risk_manager.h"

#include "support/test_helpers.h"

#include <gtest/gtest.h>

namespace {

using namespace tqsdk;

struct SimFixture {
    asio::io_context       ctx;
    SimTdGateway*          sim = nullptr;
    std::unique_ptr<TqApi> api;
    bool                   done = false;
    std::exception_ptr     error;

    explicit SimFixture(double balance = 1'000'000.0) {
        auto td_owner = std::make_unique<SimTdGateway>(ctx.get_executor(), balance);
        sim           = td_owner.get();
        ContractSpec spec;
        spec.volume_multiple    = 10.0;
        spec.price_tick         = 1.0;
        spec.margin_rate        = 0.1;
        spec.commission_per_lot = 2.0;
        sim->set_contract_spec("SHFE.rb2610", spec);
        api = std::make_unique<TqApi>(ctx.get_executor(), nullptr, std::move(td_owner));
        api->start();
        auto& quote      = api->data().quotes.get_or_create("SHFE.rb2610");
        quote.bid_price1 = 3799.0;
        quote.ask_price1 = 3800.0;
        quote.last_price = 3800.0;
        sim->on_tick("SHFE.rb2610", 3800.0);
    }

    ~SimFixture() {
        if (api) {
            api->close();
            ctx.run();
        }
    }

    void run_until(std::function<bool()> condition, int ms = 1'000) {
        done  = false;
        error = nullptr;
        asio::co_spawn(
            ctx,
            [this, condition = std::move(condition)]() -> asio::awaitable<void> {
                for (int i = 0; i < 100 && !condition(); ++i) {
                    if (!co_await api->wait_update())
                        break;
                }
            },
            [this](std::exception_ptr ep) {
                error = ep;
                done  = true;
            });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!done && std::chrono::steady_clock::now() < deadline) {
            test::pump(ctx, 10);
        }
        ASSERT_TRUE(done) << "task coroutine did not complete";
        if (error)
            std::rethrow_exception(error);
    }
};

std::int64_t net_position(TqApi& api, std::string_view symbol) {
    const auto* p = api.data().positions.find(std::string{symbol});
    if (!p)
        return 0;
    return static_cast<std::int64_t>(p->volume_long) - static_cast<std::int64_t>(p->volume_short);
}

TEST(OrderTasks, TargetPosTaskOpensAndClosesNetPosition) {
    SimFixture    fixture;
    TargetPosTask task(*fixture.api, "SHFE.rb2610", TargetPosPriceMode::ACTIVE, TargetPosOptions{});
    task.set_target_volume(2);
    fixture.run_until([&] { return net_position(*fixture.api, "SHFE.rb2610") == 2; });
    EXPECT_EQ(net_position(*fixture.api, "SHFE.rb2610"), 2);

    task.set_target_volume(0);
    fixture.run_until([&] { return net_position(*fixture.api, "SHFE.rb2610") == 0; });
    EXPECT_EQ(net_position(*fixture.api, "SHFE.rb2610"), 0);
    task.cancel();
}

TEST(OrderTasks, TargetPosTaskRejectsDuplicateOrdersWhileWorking) {
    SimFixture fixture;
    // 不推进 SimTd：限价单保持 alive，任务不能重复发送同方向订单。
    auto& quote      = fixture.api->data().quotes.get_or_create("SHFE.rb2610");
    quote.bid_price1 = 3700.0;
    quote.ask_price1 = 3800.0;
    TargetPosTask task(
        *fixture.api, "SHFE.rb2610", TargetPosPriceMode::PASSIVE, TargetPosOptions{});
    task.set_target_volume(5);
    fixture.run_until([&] { return fixture.api->data().orders.size() > 0; });
    const auto order_count = fixture.api->data().orders.size();
    test::pump(fixture.ctx, 100);
    EXPECT_EQ(fixture.api->data().orders.size(), order_count);
    task.cancel();
}

TEST(OrderTasks, TargetPosTaskCancelsAndRepricesWhenQuoteMoves) {
    SimFixture fixture;
    auto&      quote = fixture.api->data().quotes.get_or_create("SHFE.rb2610");
    quote.bid_price1 = 3799.0;
    quote.ask_price1 = 3800.0;
    TargetPosTask task(
        *fixture.api, "SHFE.rb2610", TargetPosPriceMode::PASSIVE, TargetPosOptions{});
    task.set_target_volume(1);
    fixture.run_until([&] { return fixture.api->data().orders.size() > 0; });

    quote.bid_price1 = 3805.0;
    quote.ask_price1 = 3806.0;
    fixture.sim->on_tick("SHFE.rb2610", 3805.0);
    fixture.api->notify_update();
    fixture.run_until([&] { return net_position(*fixture.api, "SHFE.rb2610") == 1; });
    EXPECT_EQ(net_position(*fixture.api, "SHFE.rb2610"), 1);
    task.cancel();
}

TEST(OrderTasks, InsertOrderTaskKeepsOrderingUntilFilled) {
    SimFixture     fixture;
    InsertOrderReq request;
    request.instrument_id = "SHFE.rb2610";
    request.direction     = Direction::BUY;
    request.offset        = Offset::OPEN;
    request.price_type    = PriceType::LIMIT;
    request.limit_price   = 3800;
    request.volume        = 2;
    InsertOrderTask task(*fixture.api, request, 3);
    fixture.run_until([&] { return !task.active(); });
    EXPECT_FALSE(task.active());
    EXPECT_EQ(task.filled_volume(), 2u);
    EXPECT_EQ(net_position(*fixture.api, "SHFE.rb2610"), 2);
}

TEST(OrderTasks, RiskRuleRejectsOrderBeforeGateway) {
    SimFixture fixture;
    fixture.api->risk_manager().add(
        std::make_shared<OpenCountsLimitRule>(1, std::vector<std::string>{"SHFE.rb2610"}));

    InsertOrderReq first;
    first.instrument_id = "SHFE.rb2610";
    first.direction     = Direction::BUY;
    first.offset        = Offset::OPEN;
    first.price_type    = PriceType::LIMIT;
    first.limit_price   = 3800;
    first.volume        = 1;

    Order* first_order = nullptr;
    asio::co_spawn(
        fixture.ctx,
        [&]() -> asio::awaitable<void> { first_order = co_await fixture.api->insert_order(first); },
        asio::detached);
    fixture.run_until([&] { return first_order != nullptr && first_order->is_dead; });
    ASSERT_NE(first_order, nullptr);
    EXPECT_FALSE(first_order->is_error);

    Order* second_order = nullptr;
    asio::co_spawn(
        fixture.ctx,
        [&]() -> asio::awaitable<void> {
            second_order = co_await fixture.api->insert_order(first);
        },
        asio::detached);
    test::pump(fixture.ctx, 50);
    ASSERT_NE(second_order, nullptr);
    EXPECT_TRUE(second_order->is_error);
    EXPECT_NE(second_order->last_msg.find("风控"), std::string::npos);
}

} // namespace
