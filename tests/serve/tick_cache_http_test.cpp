#include "tqsdk/pch.h"

#include "tqsdk/data/tick_store.h"
#include "tqsdk/gateway/mock/mock_md_gateway.h"
#include "tqsdk/gateway/mock/mock_td_gateway.h"
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
get_json(asio::any_io_executor ex, std::uint16_t port, std::string target) {
    tcp::resolver     resolver(ex);
    beast::tcp_stream stream(ex);
    const auto        results =
        co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
    co_await stream.async_connect(results, asio::use_awaitable);
    http::request<http::string_body> req{http::verb::get, target, 11};
    req.set(http::field::host, "127.0.0.1");
    co_await http::async_write(stream, req, asio::use_awaitable);
    beast::flat_buffer                buffer;
    http::response<http::string_body> res;
    co_await http::async_read(stream, buffer, res, asio::use_awaitable);
    stream.socket().shutdown(tcp::socket::shutdown_both);
    co_return std::pair<unsigned, json>{res.result_int(), json::parse(res.body())};
}

std::string temp_db() {
    return (std::filesystem::temp_directory_path() /
            ("tqsdk_tick_http_" + std::to_string(::getpid()) + ".sqlite3"))
        .string();
}

} // namespace

TEST(TickCacheHttp, ServesCachedTicksAndStatus) {
    asio::io_context io;
    auto             md_owner = std::make_unique<tqsdk::MockMdGateway>(io.get_executor());
    auto             td_owner = std::make_unique<tqsdk::MockTdGateway>(io.get_executor());
    tqsdk::TqApi     api(io.get_executor(), std::move(md_owner), std::move(td_owner));
    api.start();

    tqsdk::TickStore::Config cfg;
    cfg.enabled           = true;
    cfg.file              = temp_db();
    cfg.flush_interval_ms = 5;
    tqsdk::TickStore store(cfg);
    ASSERT_TRUE(store.ready());

    tqsdk::Tick tick;
    tick.instrument_id = "SHFE.rb2701";
    tick.datetime      = "2026-11-09 21:00:00.000000";
    tick.last_price    = 3000;
    tick.volume        = 5;
    tick.bid_price[0]  = 2999;
    tick.ask_price[0]  = 3001;
    store.append(tick);
    store.flush();

    tqsdk::serve::HttpServer server(
        &api, io.get_executor(), {}, io.get_executor(), nullptr, nullptr, &store);
    server.listen("127.0.0.1", 0);

    unsigned status_code = 0;
    json     status_body;
    unsigned ticks_code = 0;
    json     ticks_body;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto r1     = co_await get_json(io.get_executor(), server.port(), "/tick-cache/status");
            status_code = r1.first;
            status_body = std::move(r1.second);
            auto r2     = co_await get_json(
                io.get_executor(), server.port(), "/ticks/SHFE.rb2701?source=cache&limit=10");
            ticks_code = r2.first;
            ticks_body = std::move(r2.second);
            co_return;
        },
        [&](std::exception_ptr ep) {
            if (ep) {
                std::rethrow_exception(ep);
            }
            io.stop();
        });
    io.run();

    EXPECT_EQ(status_code, 200u);
    EXPECT_TRUE(status_body["ready"].get<bool>());
    EXPECT_EQ(ticks_code, 200u);
    ASSERT_TRUE(ticks_body["ticks"].is_array());
    ASSERT_EQ(ticks_body["ticks"].size(), 1u);
    EXPECT_EQ(ticks_body["ticks"][0]["last_price"], 3000);
    EXPECT_EQ(ticks_body["ticks"][0]["bid_price"][0], 2999);

    io.restart();
    server.stop();
    store.stop();
    api.close();
    io.run_for(std::chrono::milliseconds(100));
    std::filesystem::remove(cfg.file);
    std::filesystem::remove(cfg.file + "-wal");
    std::filesystem::remove(cfg.file + "-shm");
}
