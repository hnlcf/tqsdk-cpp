#include "tqsdk/pch.h"

#include "tqsdk/serve/http_server.h"
#include "tqsdk/tqapi.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

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

TEST(OptionHttp, OptionAnalyticsReturnsAtmAndGreeks) {
    const auto path = "/tmp/tqsdk_option_http_" + std::to_string(::getpid()) + ".json";
    {
        std::ofstream out(path);
        out << R"({
          "SHFE.rb2701": {"class":"FUTURE","instrument_id":"SHFE.rb2701","exchange_id":"SHFE",
                          "underlying_symbol":"SHFE.rb2701","expired":false},
          "SHFE.rb2701C3000": {"class":"FUTURE_OPTION","instrument_id":"SHFE.rb2701C3000",
             "exchange_id":"SHFE","underlying_symbol":"SHFE.rb2701","option_class":"CALL",
             "strike_price":3000,"expire_datetime":2000000000,"expired":false},
          "SHFE.rb2701P3000": {"class":"FUTURE_OPTION","instrument_id":"SHFE.rb2701P3000",
             "exchange_id":"SHFE","underlying_symbol":"SHFE.rb2701","option_class":"PUT",
             "strike_price":3000,"expire_datetime":2000000000,"expired":false},
          "SHFE.rb2701C3100": {"class":"FUTURE_OPTION","instrument_id":"SHFE.rb2701C3100",
             "exchange_id":"SHFE","underlying_symbol":"SHFE.rb2701","option_class":"CALL",
             "strike_price":3100,"expire_datetime":2000000000,"expired":false},
          "SHFE.rb2701P3100": {"class":"FUTURE_OPTION","instrument_id":"SHFE.rb2701P3100",
             "exchange_id":"SHFE","underlying_symbol":"SHFE.rb2701","option_class":"PUT",
             "strike_price":3100,"expire_datetime":2000000000,"expired":false}
        })";
    }

    asio::io_context io;
    auto             core_ex = asio::make_strand(io);
    auto             catalog =
        std::make_shared<tqsdk::InstrumentCatalog>(tqsdk::InstrumentCatalog::Config{.file = path});
    ASSERT_TRUE(catalog->load_local());
    tqsdk::TqApi              api(core_ex, nullptr, nullptr);
    tqsdk::serve::HttpServer server(&api, io.get_executor(), {}, core_ex, catalog.get());
    server.listen("127.0.0.1", 0);

    unsigned            status = 0;
    nlohmann::json      body;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto result = co_await get_json(
                io.get_executor(), server.port(),
                "/option/analytics/SHFE.rb2701?spot=3000&rate=0.02&vol=0.2&window=1");
            status = result.first;
            body   = std::move(result.second);
            io.stop();
        },
        asio::detached);
    io.run();

    EXPECT_EQ(status, 200u) << body.dump();
    EXPECT_EQ(body["atm_strike"], 3000);
    ASSERT_TRUE(body["rows"].is_array());
    EXPECT_EQ(body["rows"].size(), 4u);
    EXPECT_NE(body["rows"][0].find("greeks"), body["rows"][0].end());
    EXPECT_TRUE(body["rows"][0]["greeks"].contains("delta"));
    EXPECT_TRUE(body["rows"][0].contains("price"));
    EXPECT_EQ(body["rows"][0]["right"], "CALL");

    server.stop();
    api.close();
    io.restart();
    io.run_for(std::chrono::milliseconds(50));
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
