#include "tqsdk/pch.h"

#include "tqsdk/gateway/sim/sim_td_gateway.h"
#include "tqsdk/serve/http_server.h"
#include "tqsdk/tqapi.h"
#include "tqsdk/util/runtime.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace {

namespace beast = boost::beast;
namespace http  = boost::beast::http;
namespace asio  = boost::asio;
using tcp       = asio::ip::tcp;

asio::awaitable<void>
get_once(asio::any_io_executor ex, std::string host, uint16_t port, std::string target) {
    tcp::resolver resolver(ex);
    auto results = co_await resolver.async_resolve(host, std::to_string(port), asio::use_awaitable);

    beast::tcp_stream stream(ex);
    stream.expires_after(std::chrono::seconds(3));
    co_await stream.async_connect(results, asio::use_awaitable);

    http::request<http::empty_body> req{http::verb::get, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "tqsdk-threading-test");
    co_await http::async_write(stream, req, asio::use_awaitable);

    beast::flat_buffer                buf;
    http::response<http::string_body> res;
    co_await http::async_read(stream, buf, res, asio::use_awaitable);
    if (res.result() != http::status::ok) {
        throw std::runtime_error("unexpected HTTP status: " +
                                 std::to_string(static_cast<int>(res.result_int())));
    }

    boost::system::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_send, ec);
    co_return;
}

} // namespace

// 多 worker：HTTP I/O 在每连接 strand 上并行，路由 handler 统一在 core strand
// 读取 DataManager；32 个并发客户端应全部成功。
TEST(Threading, HttpServerWithMultipleIoThreads) {
    asio::io_context io;
    auto             io_ex   = io.get_executor();
    auto             core_ex = asio::make_strand(io);

    auto         td = std::make_unique<tqsdk::SimTdGateway>(core_ex, 1'000'000.0);
    tqsdk::TqApi api(core_ex, nullptr, std::move(td));
    api.start();

    tqsdk::serve::HttpServer server(&api, io_ex, {}, core_ex);
    server.listen("127.0.0.1", 0);
    const uint16_t port = server.port();

    constexpr int    kClients = 32;
    std::atomic<int> done{0};
    std::atomic<int> failed{0};

    for (int i = 0; i < kClients; ++i) {
        auto client_ex = asio::make_strand(io);
        auto target    = (i % 2 == 0) ? std::string("/healthz") : std::string("/account");
        asio::co_spawn(client_ex,
                       get_once(client_ex, "127.0.0.1", port, std::move(target)),
                       [&](std::exception_ptr ep) {
                           if (ep) {
                               ++failed;
                           }
                           if (++done == kClients) {
                               io.stop();
                           }
                       });
    }

    std::vector<std::thread> workers;
    for (int i = 1; i < 4; ++i) {
        workers.emplace_back([&io]() { io.run(); });
    }
    io.run();
    for (auto& t : workers) {
        t.join();
    }

    EXPECT_EQ(done.load(), kClients);
    EXPECT_EQ(failed.load(), 0);

    server.stop();
    api.close();
    io.restart();
    io.run_for(std::chrono::milliseconds(100));
}
