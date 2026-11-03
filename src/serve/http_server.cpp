#include "tqsdk/serve/http_server.h"

#include "tqsdk/domain/sentinel.h"
#include "tqsdk/serve/json_convert.h"
#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {
namespace serve {

namespace {

using json = nlohmann::json;

// JSON 字段映射统一放在 serve/json_convert.h，REST 与 WebSocket 共用。
// 路由路径解析 / URL 解码 / 参数提取均已下沉到 serve/router.h。

} // namespace

HttpServer::HttpServer(TqApi* api, asio::any_io_executor ex, WsOptions ws_options)
    : HttpServer(api, ex, std::move(ws_options), ex) {}

HttpServer::HttpServer(TqApi*                api,
                       asio::any_io_executor io_ex,
                       WsOptions             ws_options,
                       asio::any_io_executor data_ex)
    : api_(api),
      io_ex_(std::move(io_ex)),
      data_ex_(data_ex ? std::move(data_ex) : io_ex_),
      acceptor_(io_ex_),
      ws_(std::make_shared<WebSocketHub>(api_, data_ex_, io_ex_, std::move(ws_options))) {
    setup_routes();
}

void HttpServer::setup_routes() {
    // 路由以"表"的形式声明：每行 = { 方法, 路径模式, handler }。
    // 路径中 {name} 段按位置绑定到 handler 对应形参；结构化 body 自动反序列化。
    router_.add({
        // ---- 健康检查 / 只读查询 ----
        {http::verb::get, "/healthz", make_entry([]() -> asio::awaitable<json> {
             co_return json{{"ok", true}};
         })},
        {http::verb::get,
         "/quote/{symbol}",
         make_entry([this](std::string symbol) -> asio::awaitable<json> {
             // get_quote 在不存在时创建空 Quote 并触发订阅（首次访问）。
             Quote& q = api_->get_quote(symbol);
             co_return quote_to_json(q);
         })},
        {http::verb::get,
         "/kline/{symbol}/{duration_ns}",
         make_entry([this](std::string symbol, std::int64_t duration_ns) -> asio::awaitable<json> {
             KlineStream* s = api_->get_kline_serial(symbol, duration_ns);
             co_return kline_to_json(*s);
         })},
        {http::verb::get, "/account", make_entry([this]() -> asio::awaitable<json> {
             co_return account_to_json(api_->get_account());
         })},
        {http::verb::get, "/orders", make_entry([this]() -> asio::awaitable<json> {
             json out = json::object();
             for (const auto& [oid, o] : api_->data().orders) {
                 out[oid] = order_to_json(o);
             }
             co_return json{{"orders", out}};
         })},
        {http::verb::get, "/trades", make_entry([this]() -> asio::awaitable<json> {
             json out = json::object();
             for (const auto& [tid, t] : api_->data().trades) {
                 out[tid] = trade_to_json(t);
             }
             co_return json{{"trades", out}};
         })},
        {http::verb::get, "/positions", make_entry([this]() -> asio::awaitable<json> {
             json out = json::object();
             for (const auto& [sym, p] : api_->data().positions) {
                 out[sym] = position_to_json(p);
             }
             co_return json{{"positions", out}};
         })},

        // ---- 下单 / 撤单 ----
        // body 自动反序列化为 InsertOrderReq（见 trading_gateway.h 的 from_json）。
        {http::verb::post, "/order", make_entry([this](InsertOrderReq r) -> asio::awaitable<json> {
             // co_await 网关 ack；期间 io_context 继续处理行情/其它请求。
             Order* order = co_await api_->insert_order(r);
             co_return json{{"success", true},
                            {"order_id", order ? order->order_id : std::string{}},
                            {"error", ""}};
         })},
        {http::verb::delete_,
         "/order/{id}",
         make_entry([this](std::string oid) -> asio::awaitable<json> {
             CommandAck ack = co_await api_->cancel_order(CancelOrderReq{oid, ""});
             co_return json{
                 {"success", ack.success}, {"order_id", ack.order_id}, {"error", ack.error_msg}};
         })},

        // ---- WebSocket 入口：普通 GET 给 426，提示必须走 Upgrade ----
        {http::verb::get, ws_->options().path, make_entry([]() -> asio::awaitable<Response> {
             co_return Response{
                 http::status::upgrade_required, json{{"error", "websocket upgrade required"}}, {}};
         })},
    });
}

void HttpServer::listen(const std::string& address, uint16_t port) {
    tcp::endpoint             ep{boost::asio::ip::make_address(address), port};
    boost::system::error_code ec;
    acceptor_.open(ep.protocol(), ec);
    if (ec) {
        throw boost::system::system_error(ec, "HttpServer: open acceptor");
    }
    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    acceptor_.bind(ep, ec);
    if (ec) {
        throw boost::system::system_error(
            ec, "HttpServer: bind " + address + ":" + std::to_string(port));
    }
    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        throw boost::system::system_error(ec, "HttpServer: listen");
    }
    port_ = acceptor_.local_endpoint().port();

    co_spawn_logged(
        io_ex_, [this]() -> asio::awaitable<void> { co_await accept_loop(); }, "Http.accept");
    ws_->start();
    SPDLOG_INFO("[HttpServer] listening on {}:{} (ws: {})", address, port_, ws_->options().path);
}

void HttpServer::stop() noexcept {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    if (ws_) {
        ws_->stop();
    }
    boost::system::error_code ec;
    auto                      _ = acceptor_.close(ec);
}

asio::awaitable<void> HttpServer::accept_loop() {
    for (;;) {
        // 每条连接一个独立 strand：同一连接的读写串行，连接之间可并行。
        auto                      session_ex = asio::make_strand(io_ex_);
        boost::system::error_code ec;
        tcp::socket               sock(session_ex);
        co_await acceptor_.async_accept(sock, asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            if (ec == asio::error::operation_aborted) {
                co_return; // stop()
            }
            SPDLOG_WARN("[HttpServer] accept error: {}", ec.message());
            // 瞬时错误继续监听；致命错误（如 fd 耗尽）加个退避避免空转
            asio::steady_timer t(co_await asio::this_coro::executor, std::chrono::milliseconds(10));
            co_await t.async_wait(asio::use_awaitable);
            continue;
        }
        // 每连接一个协程；不等待完成（并发服务）。
        co_spawn_logged(
            session_ex,
            [this, s = std::move(sock)]() mutable -> asio::awaitable<void> {
                co_await session(std::move(s));
            },
            "Http.session");
    }
}

asio::awaitable<void> HttpServer::session(tcp::socket sock) {
    beast::flat_buffer               buffer;
    http::request<http::string_body> req;
    for (;;) {
        boost::system::error_code ec;
        co_await http::async_read(sock, buffer, req, asio::redirect_error(asio::use_awaitable, ec));
        if (ec == http::error::end_of_stream) {
            break;
        }
        if (ec) {
            break;
        }

        // ---- WebSocket 升级：在路由分发前拦截 ----
        if (beast::websocket::is_upgrade(req)) {
            std::string_view target = req.target();
            if (auto qpos = target.find('?'); qpos != std::string_view::npos) {
                target = target.substr(0, qpos);
            }

            // Hub 的 clients_/订阅表都在 data_ex_ 上；检查与接管都必须切过去。
            co_await asio::dispatch(data_ex_, asio::use_awaitable);

            http::response<http::string_body> res;
            if (target != ws_->options().path) {
                res = make_json_response(
                    http::status::not_found, json{{"error", "not found"}}, req.version());
            } else if (ws_->full()) {
                res = make_json_response(http::status::service_unavailable,
                                         json{{"error", "too many websocket clients"}},
                                         req.version());
            } else {
                // 交给 WebSocketHub 完成握手并接管 socket；本 session 结束。
                co_await ws_->accept(std::move(sock), std::move(req));
                co_return;
            }
            res.keep_alive(false);
            co_await asio::dispatch(sock.get_executor(), asio::use_awaitable);
            co_await http::async_write(sock, res, asio::redirect_error(asio::use_awaitable, ec));
            co_return;
        }

        // keep-alive 支持：根据请求决定是否关闭。
        const bool close_conn = req.need_eof();

        // handler 读 DataManager / 调 TqApi，统一在 data_ex_ 上执行。
        co_await asio::dispatch(data_ex_, asio::use_awaitable);

        http::response<http::string_body> res;
        try {
            res = co_await handle(std::move(req));
        } catch (const std::exception& e) {
            res = make_json_response(
                http::status::internal_server_error, json{{"error", e.what()}}, 11);
        }

        if (close_conn) {
            res.keep_alive(false);
        }
        // 写回 socket 前切回连接自己的 executor/strand，避免不同连接互相串行。
        co_await asio::dispatch(sock.get_executor(), asio::use_awaitable);
        co_await http::async_write(sock, res, asio::redirect_error(asio::use_awaitable, ec));
        if (ec || close_conn) {
            break;
        }
        req = {}; // 复用前清空
    }

    boost::system::error_code ec;
    sock.shutdown(tcp::socket::shutdown_send, ec);
    co_return;
}

http::response<http::string_body>
HttpServer::make_json_response(http::status code, const nlohmann::json& body, unsigned version) {
    http::response<http::string_body> res{code, version};
    res.set(http::field::server, "tqsdk-node");
    res.set(http::field::content_type, "application/json; charset=utf-8");
    res.keep_alive(true);
    res.body() = body.dump();
    res.prepare_payload();
    return res;
}

asio::awaitable<http::response<http::string_body>>
HttpServer::handle(http::request<http::string_body> req) {
    const unsigned version = req.version();
    // 路由分发：Router 命中路由，按 handler 形参类型自动提取路径参数 / body，
    // 并把 handler 返回值（json / Response）与 HttpError 转成统一 Response。
    Response                          r   = co_await router_.dispatch(req);
    http::response<http::string_body> res = make_json_response(r.status, r.body, version);
    for (const auto& [k, v] : r.headers) {
        res.set(k, v);
    }
    co_return res;
}

} // namespace serve
} // namespace tqsdk
