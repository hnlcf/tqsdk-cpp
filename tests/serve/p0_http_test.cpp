#include "tqsdk/pch.h"

#include "tqsdk/gateway/sim/sim_td_gateway.h"
#include "tqsdk/serve/http_server.h"
#include "tqsdk/tqapi.h"

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

} // namespace

TEST(P0Http, RiskAndTaskRoutesAreWired) {
    asio::io_context io;
    auto             core_ex = asio::make_strand(io);

    auto         td = std::make_unique<tqsdk::SimTdGateway>(core_ex, 1'000'000.0);
    tqsdk::TqApi api(core_ex, nullptr, std::move(td));
    api.start();

    tqsdk::serve::HttpServer server(&api, io.get_executor(), {}, core_ex);
    server.listen("127.0.0.1", 0);
    const auto port = server.port();

    unsigned    risk_status = 0;
    unsigned    task_create = 0;
    unsigned    tasks_list  = 0;
    json        task_body;
    json        task_list;
    std::string task_id;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            {
                auto [status, body] = co_await request_json(
                    io.get_executor(),
                    port,
                    http::verb::post,
                    "/risk/rules",
                    json{{"type", "OPEN_COUNT_LIMIT"}, {"limit", 1}, {"symbols", {"SHFE.rb2701"}}});
                risk_status = status;
                EXPECT_EQ(body["success"], true);
            }
            {
                auto [status, body] =
                    co_await request_json(io.get_executor(), port, http::verb::get, "/risk/status");
                EXPECT_EQ(status, 200u);
                EXPECT_EQ(body["rules"], 1);
            }
            {
                auto [status, body] = co_await request_json(
                    io.get_executor(), port, http::verb::delete_, "/risk/rules");
                EXPECT_EQ(status, 200u);
                EXPECT_EQ(body["rules"], 0);
            }
            {
                auto [status, body] = co_await request_json(
                    io.get_executor(),
                    port,
                    http::verb::post,
                    "/task/target",
                    json{{"symbol", "SHFE.rb2701"}, {"target_volume", 0}, {"mode", "ACTIVE"}});
                task_create = status;
                task_body   = body;
                task_id     = body.value("task_id", std::string{});
            }
            {
                auto [status, body] =
                    co_await request_json(io.get_executor(), port, http::verb::get, "/tasks");
                tasks_list = status;
                task_list  = body;
            }
            co_return;
        },
        [&](std::exception_ptr ep) {
            if (ep) {
                std::rethrow_exception(ep);
            }
            io.stop();
        });
    io.run();

    EXPECT_EQ(risk_status, 200u);
    EXPECT_EQ(task_create, 200u);
    EXPECT_TRUE(task_body["success"]);
    ASSERT_FALSE(task_id.empty());
    EXPECT_EQ(tasks_list, 200u);
    ASSERT_TRUE(task_list["targets"].is_array());
    EXPECT_EQ(task_list["targets"].size(), 1u);
    EXPECT_EQ(task_list["targets"][0]["task_id"], task_id);

    io.restart();
    server.stop();
    api.close();
    io.run_for(std::chrono::milliseconds(100));
}
