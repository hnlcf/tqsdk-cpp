#include "tqsdk/pch.h"

#include "tqsdk/gateway/sim/sim_td_gateway.h"
#include "tqsdk/multiaccount/multi_account_manager.h"
#include "tqsdk/serve/http_server.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <gtest/gtest.h>

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = boost::beast::http;
using tcp       = asio::ip::tcp;
using json      = nlohmann::json;

namespace {

asio::awaitable<std::pair<unsigned, json>>
request_json(asio::any_io_executor ex, std::uint16_t port, http::verb method, std::string target,
             json body = nullptr) {
    tcp::resolver     resolver(ex);
    beast::tcp_stream stream(ex);
    const auto        results =
        co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
    co_await stream.async_connect(results, asio::use_awaitable);

    http::request<http::string_body> req{method, target, 11};
    req.set(http::field::host, "127.0.0.1");
    if (!body.is_null()) {
        req.set(http::field::content_type, "application/json");
        req.body() = body.dump();
        req.prepare_payload();
    }
    co_await http::async_write(stream, req, asio::use_awaitable);
    beast::flat_buffer                buffer;
    http::response<http::string_body> res;
    co_await http::async_read(stream, buffer, res, asio::use_awaitable);
    stream.socket().shutdown(tcp::socket::shutdown_both);
    co_return std::pair<unsigned, json>{res.result_int(), json::parse(res.body())};
}

struct SimAccount {
    std::shared_ptr<tqsdk::TqApi> api;
};

SimAccount make_account(asio::io_context& io, double balance) {
    auto td_owner = std::make_unique<tqsdk::SimTdGateway>(io.get_executor(), balance);
    tqsdk::ContractSpec spec;
    spec.volume_multiple = 10;
    spec.price_tick      = 1;
    spec.margin_rate     = 0.1;
    td_owner->set_contract_spec("SHFE.rb2701", spec);
    td_owner->on_tick("SHFE.rb2701", 3000);
    auto api = std::make_shared<tqsdk::TqApi>(io.get_executor(), nullptr, std::move(td_owner));
    api->start();
    return SimAccount{std::move(api)};
}

} // namespace

TEST(MultiAccountHttp, RoutesTradingAndSnapshotsByAccount) {
    asio::io_context io;
    auto             a = make_account(io, 1'000'000);
    auto             b = make_account(io, 2'000'000);

    tqsdk::MultiAccountManager manager;
    ASSERT_TRUE(manager.add_account("A", a.api));
    ASSERT_TRUE(manager.add_account("B", b.api));

    tqsdk::serve::HttpServer server(a.api.get(), io.get_executor(), {}, io.get_executor(),
                                    nullptr, &manager);
    server.listen("127.0.0.1", 0);

    unsigned       accounts_status = 0;
    json           accounts_body;
    unsigned       order_status = 0;
    json           order_body;
    unsigned       b_positions_status = 0;
    json           b_positions_body;
    unsigned       a_positions_status = 0;
    json           a_positions_body;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto r1 = co_await request_json(
                io.get_executor(), server.port(), http::verb::get, "/accounts");
            accounts_status = r1.first;
            accounts_body   = std::move(r1.second);

            auto r2 = co_await request_json(
                io.get_executor(), server.port(), http::verb::post, "/order?account=B",
                json{{"instrument_id", "SHFE.rb2701"},
                     {"direction", "BUY"},
                     {"offset", "OPEN"},
                     {"price_type", "ANY"},
                     {"limit_price", 0},
                     {"volume", 1}});
            order_status = r2.first;
            order_body   = std::move(r2.second);

            auto r3 = co_await request_json(
                io.get_executor(), server.port(), http::verb::get, "/positions?account=B");
            b_positions_status = r3.first;
            b_positions_body   = std::move(r3.second);

            auto r4 = co_await request_json(
                io.get_executor(), server.port(), http::verb::get, "/positions?account=A");
            a_positions_status = r4.first;
            a_positions_body   = std::move(r4.second);
            co_return;
        },
        [&](std::exception_ptr ep) {
            if (ep) std::rethrow_exception(ep);
            io.stop();
        });
    io.run();

    EXPECT_EQ(accounts_status, 200u);
    ASSERT_TRUE(accounts_body["accounts"].is_array());
    EXPECT_EQ(accounts_body["accounts"].size(), 2u);
    EXPECT_EQ(accounts_body["default"], "A");

    EXPECT_EQ(order_status, 200u) << order_body.dump();
    EXPECT_TRUE(order_body["success"]) << order_body.dump();
    EXPECT_EQ(b_positions_status, 200u);
    ASSERT_TRUE(b_positions_body["positions"].contains("SHFE.rb2701"));
    EXPECT_EQ(b_positions_body["positions"]["SHFE.rb2701"]["volume_long"], 1);
    EXPECT_EQ(a_positions_status, 200u);
    EXPECT_FALSE(a_positions_body["positions"].contains("SHFE.rb2701"));

    io.restart();
    server.stop();
    manager.close();
    io.run_for(std::chrono::milliseconds(100));
}
