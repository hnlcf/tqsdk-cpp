#include "tqsdk/pch.h"

#include "tqsdk/gateway/sim/sim_td_gateway.h"
#include "tqsdk/multiaccount/multi_account_manager.h"

#include "support/test_helpers.h"

#include <gtest/gtest.h>

namespace {

using namespace tqsdk;

struct SimAccount {
    std::shared_ptr<TqApi> api;
    SimTdGateway*          td = nullptr;
};

SimAccount make_account(asio::io_context&  io,
                        const std::string& symbol,
                        double             balance,
                        const std::string& key) {
    auto         td_owner = std::make_unique<SimTdGateway>(io.get_executor(), balance);
    auto*        td       = td_owner.get();
    ContractSpec spec;
    spec.volume_multiple    = 10.0;
    spec.price_tick         = 1.0;
    spec.margin_rate        = 0.1;
    spec.commission_per_lot = 1.0;
    td->set_contract_spec(symbol, spec);
    td->on_tick(symbol, 3000.0);
    auto api = std::make_shared<TqApi>(io.get_executor(), nullptr, std::move(td_owner));
    api->start();
    (void)key;
    return SimAccount{api, td};
}

TEST(MultiAccount, RoutesOrdersByAccountAndAggregatesSnapshots) {
    asio::io_context io;
    auto             a = make_account(io, "SHFE.rb2701", 1'000'000.0, "A");
    auto             b = make_account(io, "SHFE.rb2701", 2'000'000.0, "B");

    MultiAccountManager manager;
    ASSERT_TRUE(manager.add_account("A", a.api));
    ASSERT_TRUE(manager.add_account("B", b.api));
    EXPECT_FALSE(manager.add_account("A", a.api));
    ASSERT_EQ(manager.account_ids().size(), 2u);

    InsertOrderReq request;
    request.instrument_id = "SHFE.rb2701";
    request.direction     = Direction::BUY;
    request.offset        = Offset::OPEN;
    request.price_type    = PriceType::ANY;
    request.volume        = 1;

    bool               done = false;
    std::exception_ptr error;
    std::string        a_order;
    std::string        b_order;
    Order*             unknown = nullptr;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto* first  = co_await manager.insert_order("A", request);
            auto* second = co_await manager.insert_order("B", request);
            unknown      = co_await manager.insert_order("missing", request);
            if (first)
                a_order = first->order_id;
            if (second)
                b_order = second->order_id;
            co_await a.api->wait_update();
            co_await b.api->wait_update();
        },
        [&](std::exception_ptr ep) {
            error = ep;
            done  = true;
        });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        test::pump(io, 10);
    }
    ASSERT_TRUE(done);
    if (error)
        std::rethrow_exception(error);
    EXPECT_FALSE(a_order.empty());
    EXPECT_FALSE(b_order.empty());
    EXPECT_EQ(unknown, nullptr);
    EXPECT_EQ(a.api->data().positions.find("SHFE.rb2701")->volume_long, 1u);
    EXPECT_EQ(b.api->data().positions.find("SHFE.rb2701")->volume_long, 1u);

    const auto snap = manager.snapshot();
    ASSERT_TRUE(snap.contains("A"));
    ASSERT_TRUE(snap.contains("B"));
    EXPECT_EQ(snap["A"]["positions"].size(), 1u);
    EXPECT_EQ(snap["B"]["positions"].size(), 1u);
    EXPECT_DOUBLE_EQ(snap["A"]["account"]["balance"].get<double>(), 999'999.0);
    EXPECT_DOUBLE_EQ(snap["B"]["account"]["balance"].get<double>(), 1'999'999.0);

    manager.close();
    test::pump(io, 20);
}

TEST(MultiAccount, CloseRetiresApisUntilManagerDestruction) {
    asio::io_context     io;
    auto                 account = make_account(io, "SHFE.rb2701", 1'000'000.0, "retire");
    std::weak_ptr<TqApi> weak    = account.api;

    {
        MultiAccountManager manager;
        ASSERT_TRUE(manager.add_account("A", account.api));
        account.api.reset();
        manager.close();
        // close() only closes; it must not destroy a TqApi whose coroutines may
        // still be queued for io_context drain.
        EXPECT_FALSE(weak.expired());
    }

    EXPECT_TRUE(weak.expired());
}

} // namespace
