#include "tqsdk/pch.h"

#include "tqsdk/gateway/sim/sim_td_gateway.h"
#include "tqsdk/serve/http_server.h"
#include "tqsdk/tqapi.h"

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = boost::beast::http;
using tcp       = asio::ip::tcp;

namespace {

asio::awaitable<std::pair<unsigned, nlohmann::json>>
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
    co_return std::pair<unsigned, nlohmann::json>{res.result_int(),
                                                  nlohmann::json::parse(res.body())};
}

} // namespace

TEST(Readiness, SimGatewayIsReadyAndExposesMetrics) {
    asio::io_context io;
    auto             core_ex = asio::make_strand(io);

    auto         td = std::make_unique<tqsdk::SimTdGateway>(core_ex, 1'000'000.0);
    tqsdk::TqApi api(core_ex, nullptr, std::move(td));
    api.start();

    EXPECT_TRUE(api.ready());
    const auto ready = api.readiness_json();
    EXPECT_TRUE(ready["ready"]);
    EXPECT_FALSE(ready["md"]["present"]);
    EXPECT_TRUE(ready["td"]["present"]);
    EXPECT_TRUE(ready["td"]["ready"]);
    EXPECT_EQ(ready["td"]["phase"], "unknown");
    EXPECT_FALSE(ready["td"]["expected_disconnect"]);

    tqsdk::serve::HttpServer server(&api, io.get_executor(), {}, core_ex);
    server.listen("127.0.0.1", 0);
    const auto port = server.port();

    unsigned       status = 0;
    nlohmann::json body;
    unsigned       health_status = 0;
    nlohmann::json health_body;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto [hs, hb] = co_await get_json(io.get_executor(), port, "/healthz");
            health_status = hs;
            health_body   = std::move(hb);
            auto [s, b]   = co_await get_json(io.get_executor(), port, "/readyz");
            status        = s;
            body          = std::move(b);
            io.stop();
            co_return;
        },
        asio::detached);
    io.run();
    EXPECT_EQ(health_status, 200u);
    EXPECT_EQ(health_body["package"], "tqsdk-cpp");
    EXPECT_EQ(health_body["version"], tqsdk::kVersion);
    EXPECT_EQ(health_body["package_aliases"].size(), 4u);
    EXPECT_EQ(status, 200u);
    EXPECT_TRUE(body["ready"]);
    EXPECT_TRUE(body["td"]["ready"]);

    io.restart();
    api.close();
    server.stop();
    io.run_for(std::chrono::milliseconds(100));
}
