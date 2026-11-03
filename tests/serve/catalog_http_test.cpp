#include "tqsdk/pch.h"

#include "tqsdk/instrument/instrument_catalog.h"
#include "tqsdk/serve/http_server.h"
#include "tqsdk/tqapi.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <unistd.h>
#include <fstream>

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = boost::beast::http;
using tcp       = asio::ip::tcp;

namespace {

asio::awaitable<std::pair<unsigned, nlohmann::json>>
get_json(asio::any_io_executor ex, uint16_t port, std::string target) {
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

TEST(CatalogHttp, QueryNumbersAreStrictAndBounded) {
    const auto path = "/tmp/tqsdk_catalog_http_" + std::to_string(::getpid()) + ".json";
    {
        std::ofstream out(path);
        out << R"({
          "A": {"class":"FUTURE","instrument_id":"A","expired":false},
          "B": {"class":"FUTURE","instrument_id":"B","expired":false}
        })";
    }

    asio::io_context io;
    auto             core_ex = asio::make_strand(io);
    auto             catalog =
        std::make_shared<tqsdk::InstrumentCatalog>(tqsdk::InstrumentCatalog::Config{.file = path});
    ASSERT_TRUE(catalog->load_local());

    tqsdk::TqApi api(core_ex, nullptr, nullptr);
    api.start();
    tqsdk::serve::HttpServer server(&api, io.get_executor(), {}, core_ex, catalog.get());
    server.listen("127.0.0.1", 0);
    const auto port = server.port();

    std::vector<std::pair<unsigned, nlohmann::json>> responses;
    asio::co_spawn(
        core_ex,
        [&]() -> asio::awaitable<void> {
            for (const auto* target : {
                     "/catalog/instruments?limit=1&offset=-1",
                     "/catalog/instruments?limit=999999999999999999999999",
                     "/catalog/instruments?limit=abc&offset=abc",
                     "/catalog/instruments?include_expired=TRUE",
                 }) {
                responses.push_back(co_await get_json(core_ex, port, target));
            }
            io.stop();
            co_return;
        },
        asio::detached);
    io.run();

    ASSERT_EQ(responses.size(), 4u);
    for (const auto& [status, body] : responses) {
        EXPECT_EQ(status, 200u) << body.dump();
        EXPECT_TRUE(body.contains("instruments")) << body.dump();
        EXPECT_TRUE(body["instruments"].is_array()) << body.dump();
        EXPECT_LE(body["instruments"].size(), 2u) << body.dump();
    }
    EXPECT_EQ(responses[0].second["instruments"].size(), 1u);
    EXPECT_EQ(responses[1].second["instruments"].size(), 2u);
    EXPECT_EQ(responses[2].second["instruments"].size(), 2u);
    EXPECT_EQ(responses[3].second["instruments"].size(), 2u);

    server.stop();
    api.close();
    io.restart();
    io.run_for(std::chrono::milliseconds(50));
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
