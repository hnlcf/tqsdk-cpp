#include "tqsdk/util/http_download.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

namespace tqsdk::util {

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = boost::beast::http;

namespace {

struct UrlParts {
    bool        tls = true;
    std::string host;
    std::string port;
    std::string target;
};

UrlParts parse_url(std::string_view url) {
    UrlParts out;
    if (url.starts_with("https://")) {
        out.tls  = true;
        out.port = "443";
        url.remove_prefix(8);
    } else if (url.starts_with("http://")) {
        out.tls  = false;
        out.port = "80";
        url.remove_prefix(7);
    } else {
        throw std::runtime_error("unsupported URL scheme: " + std::string{url});
    }
    const auto slash     = url.find('/');
    auto       host_port = url.substr(0, slash);
    out.target           = slash == std::string_view::npos ? "/" : std::string{url.substr(slash)};
    if (const auto colon = host_port.rfind(':'); colon != std::string_view::npos) {
        out.host = std::string{host_port.substr(0, colon)};
        out.port = std::string{host_port.substr(colon + 1)};
    } else {
        out.host = std::string{host_port};
    }
    if (out.host.empty()) {
        throw std::runtime_error("empty host in URL");
    }
    return out;
}

} // namespace

std::string
http_get(std::string_view url, std::string_view user_agent, std::chrono::milliseconds timeout) {
    const auto                      parsed = parse_url(url);
    asio::io_context                io;
    asio::ip::tcp::resolver         resolver(io);
    const auto                      endpoints = resolver.resolve(parsed.host, parsed.port);
    http::request<http::empty_body> req{http::verb::get, parsed.target, 11};
    req.set(http::field::host, parsed.host);
    req.set(http::field::user_agent, user_agent);
    req.set(http::field::accept, "application/json");

    beast::flat_buffer                buffer;
    http::response<http::string_body> res;

    if (parsed.tls) {
        asio::ssl::context ctx{asio::ssl::context::tls_client};
        ctx.set_default_verify_paths();
        beast::ssl_stream<beast::tcp_stream> stream(io, ctx);
        stream.set_verify_mode(asio::ssl::verify_peer);
        stream.set_verify_callback(asio::ssl::host_name_verification(parsed.host));
        if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed.host.c_str())) {
            throw std::runtime_error("SSL_set_tlsext_host_name failed");
        }
        beast::get_lowest_layer(stream).expires_after(timeout);
        beast::get_lowest_layer(stream).connect(endpoints);
        stream.handshake(asio::ssl::stream_base::client);
        http::write(stream, req);
        http::read(stream, buffer, res);
        // 不等待 TLS close_notify：部分 CDN 会保持长连接，sync shutdown 可能阻塞
        // 后台刷新线程。socket 析构时会关闭连接。
    } else {
        beast::tcp_stream stream(io);
        stream.expires_after(timeout);
        stream.connect(endpoints);
        http::write(stream, req);
        http::read(stream, buffer, res);
    }

    if (res.result() != http::status::ok) {
        throw std::runtime_error("URL returned HTTP " + std::to_string(res.result_int()));
    }
    return std::move(res.body());
}

} // namespace tqsdk::util
