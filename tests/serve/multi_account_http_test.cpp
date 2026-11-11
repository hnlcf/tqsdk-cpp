#include "tqsdk/pch.h"

#include "tqsdk/gateway/sim/sim_td_gateway.h"
#include "tqsdk/multiaccount/multi_account_manager.h"
#include "tqsdk/serve/http_server.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <gtest/gtest.h>

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = boost::beast::http;
using tcp       = asio::ip::tcp;
using json      = nlohmann::json;

namespace {

asio::awaitable<std::pair<unsigned, json>> request_json(asio::any_io_executor ex,
                                                        std::uint16_t         port,
                                                        http::verb            method,
                                                        std::string           target,
                                                        json                  body = nullptr) {
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

    tqsdk::serve::HttpServer server(
        a.api.get(), io.get_executor(), {}, io.get_executor(), nullptr, &manager);
    server.listen("127.0.0.1", 0);

    unsigned accounts_status = 0;
    json     accounts_body;
    unsigned order_status = 0;
    json     order_body;
    unsigned b_positions_status = 0;
    json     b_positions_body;
    unsigned a_positions_status = 0;
    json     a_positions_body;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto r1 = co_await request_json(
                io.get_executor(), server.port(), http::verb::get, "/accounts");
            accounts_status = r1.first;
            accounts_body   = std::move(r1.second);

            auto r2      = co_await request_json(io.get_executor(),
                                                 server.port(),
                                                 http::verb::post,
                                                 "/order?account=B",
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
            if (ep)
                std::rethrow_exception(ep);
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

namespace {

namespace beast_ws = boost::beast::websocket;

class WsClient {
public:
    explicit WsClient(asio::io_context& ctx)
        : ws_(ctx) {}

    asio::awaitable<void> connect(std::string host, uint16_t port, std::string path) {
        asio::ip::tcp::endpoint ep{asio::ip::make_address(host), port};
        co_await ws_.next_layer().async_connect(ep, asio::use_awaitable);
        co_await ws_.async_handshake(host, path, asio::use_awaitable);
    }

    asio::awaitable<json> read_json() {
        buffer_.clear();
        co_await ws_.async_read(buffer_, asio::use_awaitable);
        std::string text = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());
        co_return json::parse(text);
    }

    asio::awaitable<void> send_json(json value) {
        std::string text = value.dump();
        co_await ws_.async_write(asio::buffer(text), asio::use_awaitable);
    }

    asio::awaitable<void> close() {
        boost::system::error_code ec;
        co_await ws_.async_close(beast_ws::close_reason(beast_ws::close_code::normal),
                                 asio::redirect_error(asio::use_awaitable, ec));
    }

private:
    beast_ws::stream<asio::ip::tcp::socket> ws_;
    beast::flat_buffer                      buffer_;
};

asio::awaitable<json> read_snapshot(WsClient& client) {
    for (int i = 0; i < 8; ++i) {
        auto message = co_await client.read_json();
        if (message.value("type", "") == "snapshot") {
            co_return message["data"];
        }
    }
    co_return json::object();
}

} // namespace

TEST(MultiAccountHttp, WebSocketRoutesByAccount) {
    asio::io_context io;
    auto             a = make_account(io, 1'000'000);
    auto             b = make_account(io, 2'000'000);

    tqsdk::MultiAccountManager manager;
    ASSERT_TRUE(manager.add_account("A", a.api));
    ASSERT_TRUE(manager.add_account("B", b.api));

    tqsdk::serve::HttpServer server(
        a.api.get(), io.get_executor(), {}, io.get_executor(), nullptr, &manager);
    server.listen("127.0.0.1", 0);

    json                      b_snapshot;
    json                      a_snapshot;
    std::pair<unsigned, json> order_result;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            order_result = co_await request_json(io.get_executor(),
                                                 server.port(),
                                                 http::verb::post,
                                                 "/order?account=B",
                                                 json{{"instrument_id", "SHFE.rb2701"},
                                                      {"direction", "BUY"},
                                                      {"offset", "OPEN"},
                                                      {"price_type", "ANY"},
                                                      {"limit_price", 0},
                                                      {"volume", 1}});

            WsClient b_ws(io);
            co_await b_ws.connect("127.0.0.1", server.port(), "/stream?account=B");
            co_await b_ws.read_json(); // welcome
            co_await b_ws.send_json(json{{"op", "subscribe"},
                                         {"symbols", json::array()},
                                         {"klines", json::array()},
                                         {"topics", {"account", "positions"}}});
            b_snapshot = co_await read_snapshot(b_ws);
            co_await b_ws.close();

            WsClient a_ws(io);
            co_await a_ws.connect("127.0.0.1", server.port(), "/stream?account=A");
            co_await a_ws.read_json(); // welcome
            co_await a_ws.send_json(json{{"op", "subscribe"},
                                         {"symbols", json::array()},
                                         {"klines", json::array()},
                                         {"topics", {"account", "positions"}}});
            a_snapshot = co_await read_snapshot(a_ws);
            co_await a_ws.close();
            server.drop_account_hub("B");
            co_return;
        },
        [&](std::exception_ptr ep) {
            if (ep)
                std::rethrow_exception(ep);
            io.stop();
        });
    io.run();

    EXPECT_EQ(order_result.first, 200u) << order_result.second.dump();
    ASSERT_TRUE(b_snapshot.contains("positions"));
    ASSERT_TRUE(b_snapshot["positions"].contains("SHFE.rb2701"));
    EXPECT_EQ(b_snapshot["positions"]["SHFE.rb2701"]["volume_long"], 1);
    ASSERT_TRUE(a_snapshot.contains("positions"));
    EXPECT_FALSE(a_snapshot["positions"].contains("SHFE.rb2701"));

    io.restart();
    server.stop();
    manager.close();
    io.run_for(std::chrono::milliseconds(100));
}

TEST(MultiAccountHttp, TasksAndRiskAreAccountScoped) {
    asio::io_context io;
    auto             a = make_account(io, 1'000'000);
    auto             b = make_account(io, 2'000'000);

    tqsdk::MultiAccountManager manager;
    ASSERT_TRUE(manager.add_account("A", a.api));
    ASSERT_TRUE(manager.add_account("B", b.api));
    tqsdk::serve::HttpServer server(
        a.api.get(), io.get_executor(), {}, io.get_executor(), nullptr, &manager);
    server.listen("127.0.0.1", 0);

    unsigned    create_status = 0;
    json        create_body;
    unsigned    b_tasks_status = 0;
    json        b_tasks_body;
    unsigned    a_tasks_status = 0;
    json        a_tasks_body;
    unsigned    b_risk_status = 0;
    json        b_risk_body;
    unsigned    a_risk_status = 0;
    json        a_risk_body;
    std::string task_id;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto r1 = co_await request_json(
                io.get_executor(),
                server.port(),
                http::verb::post,
                "/task/target?account=B",
                json{{"symbol", "SHFE.rb2701"}, {"target_volume", 0}, {"mode", "ACTIVE"}});
            create_status = r1.first;
            create_body   = std::move(r1.second);
            task_id       = create_body.value("task_id", "");

            auto r2 = co_await request_json(
                io.get_executor(), server.port(), http::verb::get, "/tasks?account=B");
            b_tasks_status = r2.first;
            b_tasks_body   = std::move(r2.second);

            auto r3 = co_await request_json(
                io.get_executor(), server.port(), http::verb::get, "/tasks?account=A");
            a_tasks_status = r3.first;
            a_tasks_body   = std::move(r3.second);

            auto r4 = co_await request_json(
                io.get_executor(),
                server.port(),
                http::verb::post,
                "/risk/rules?account=B",
                json{{"type", "ORDER_RATE_LIMIT"}, {"limit", 1}, {"window_ms", 1000}});
            b_risk_status = r4.first;
            b_risk_body   = std::move(r4.second);

            auto r5 = co_await request_json(
                io.get_executor(), server.port(), http::verb::get, "/risk/status?account=A");
            a_risk_status = r5.first;
            a_risk_body   = std::move(r5.second);
            co_return;
        },
        [&](std::exception_ptr ep) {
            if (ep)
                std::rethrow_exception(ep);
            io.stop();
        });
    io.run();

    EXPECT_EQ(create_status, 200u) << create_body.dump();
    ASSERT_FALSE(task_id.empty());
    EXPECT_EQ(create_body["account"], "B");
    EXPECT_EQ(b_tasks_status, 200u) << b_tasks_body.dump();
    ASSERT_EQ(b_tasks_body["targets"].size(), 1u);
    EXPECT_EQ(b_tasks_body["targets"][0]["task_id"], task_id);
    EXPECT_EQ(a_tasks_status, 200u) << a_tasks_body.dump();
    EXPECT_EQ(a_tasks_body["targets"].size(), 0u);
    EXPECT_EQ(b_risk_status, 200u) << b_risk_body.dump();
    EXPECT_EQ(b_risk_body["rules"], 1u);
    EXPECT_EQ(a_risk_status, 200u) << a_risk_body.dump();
    EXPECT_EQ(a_risk_body["rules"], 0u);

    io.restart();
    server.stop();
    manager.close();
    io.run_for(std::chrono::milliseconds(100));
}

TEST(MultiAccountHttp, TaskRegistryIdsArePerAccount) {
    asio::io_context io;
    auto             a = make_account(io, 1'000'000);
    auto             b = make_account(io, 2'000'000);

    tqsdk::MultiAccountManager manager;
    ASSERT_TRUE(manager.add_account("A", a.api));
    ASSERT_TRUE(manager.add_account("B", b.api));
    tqsdk::serve::HttpServer server(
        a.api.get(), io.get_executor(), {}, io.get_executor(), nullptr, &manager);
    server.listen("127.0.0.1", 0);

    std::string a_id;
    std::string b_id;
    json        a_tasks;
    json        b_tasks;
    unsigned    a_create_status = 0;
    unsigned    b_create_status = 0;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            const auto target =
                json{{"symbol", "SHFE.rb2701"}, {"target_volume", 0}, {"mode", "ACTIVE"}};
            auto ra         = co_await request_json(io.get_executor(),
                                                    server.port(),
                                                    http::verb::post,
                                                    "/task/target?account=A",
                                                    target);
            auto rb         = co_await request_json(io.get_executor(),
                                                    server.port(),
                                                    http::verb::post,
                                                    "/task/target?account=B",
                                                    target);
            a_create_status = ra.first;
            b_create_status = rb.first;
            a_id            = ra.second.value("task_id", "");
            b_id            = rb.second.value("task_id", "");

            auto ta = co_await request_json(
                io.get_executor(), server.port(), http::verb::get, "/tasks?account=A");
            auto tb = co_await request_json(
                io.get_executor(), server.port(), http::verb::get, "/tasks?account=B");
            a_tasks = std::move(ta.second);
            b_tasks = std::move(tb.second);

            co_return;
        },
        [&](std::exception_ptr ep) {
            if (ep)
                std::rethrow_exception(ep);
            io.stop();
        });
    io.run();

    EXPECT_EQ(a_create_status, 200u);
    EXPECT_EQ(b_create_status, 200u);
    EXPECT_FALSE(a_id.empty());
    EXPECT_FALSE(b_id.empty());
    EXPECT_EQ(a_id, b_id); // ids are local to each account registry
    ASSERT_EQ(a_tasks["targets"].size(), 1u);
    ASSERT_EQ(b_tasks["targets"].size(), 1u);
    EXPECT_EQ(a_tasks["targets"][0]["task_id"], a_id);
    EXPECT_EQ(b_tasks["targets"][0]["task_id"], b_id);

    io.restart();
    server.stop();
    manager.close();
    io.run_for(std::chrono::milliseconds(100));
}

TEST(MultiAccountHttp, RemovedAccountDropsHubAndRejectsStream) {
    asio::io_context           io;
    auto                       a = make_account(io, 1'000'000);
    auto                       b = make_account(io, 2'000'000);
    tqsdk::MultiAccountManager manager;
    ASSERT_TRUE(manager.add_account("A", a.api));
    ASSERT_TRUE(manager.add_account("B", b.api));

    tqsdk::serve::HttpServer server(
        a.api.get(), io.get_executor(), {}, io.get_executor(), nullptr, &manager);
    server.listen("127.0.0.1", 0);

    // Added after HttpServer construction; no gateway/background loop needed for
    // the WS routing test. The removal callback must drop the lazy hub.
    auto c = std::make_shared<tqsdk::TqApi>(io.get_executor(), nullptr, nullptr);
    ASSERT_TRUE(manager.add_account("C", c));

    bool rejected_after_remove = false;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            WsClient ws(io);
            co_await ws.connect("127.0.0.1", server.port(), "/stream?account=C");
            co_await ws.read_json();
            co_await ws.send_json(json{{"op", "subscribe"},
                                       {"symbols", json::array()},
                                       {"klines", json::array()},
                                       {"topics", {"account"}}});
            co_await read_snapshot(ws);
            co_await ws.close();
            co_return;
        },
        [&](std::exception_ptr ep) {
            if (ep)
                std::rethrow_exception(ep);
            io.stop();
        });
    io.run();

    manager.remove_account("C");
    io.restart();
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                WsClient ws(io);
                co_await ws.connect("127.0.0.1", server.port(), "/stream?account=C");
            } catch (const std::exception&) {
                rejected_after_remove = true;
            }
            co_return;
        },
        [&](std::exception_ptr ep) {
            if (ep)
                std::rethrow_exception(ep);
            io.stop();
        });
    io.run();
    EXPECT_TRUE(rejected_after_remove);

    io.restart();
    server.stop();
    manager.close();
    c.reset();
    io.run_for(std::chrono::milliseconds(200));
}
