#include "tqsdk/serve/http_server.h"
#include "tqsdk/trading/holiday_calendar.h"

#include "tqsdk/domain/sentinel.h"
#include "tqsdk/serve/json_convert.h"
#include "tqsdk/util/json_value.h"
#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {
namespace serve {

HttpServer::HttpServer(TqApi*                      api,
                       asio::any_io_executor       ex,
                       WsOptions                   ws_options,
                       InstrumentCatalog*          catalog,
                       MultiAccountManager*        accounts,
                       TickStore*                  tick_store,
                       AuthOptions                 auth,
                       std::shared_ptr<audit::Log> audit,
                       ContinuousTable*            continuous)
    : HttpServer(api,
                 ex,
                 std::move(ws_options),
                 ex,
                 catalog,
                 accounts,
                 tick_store,
                 std::move(auth),
                 std::move(audit),
                 continuous) {}

HttpServer::HttpServer(TqApi*                      api,
                       asio::any_io_executor       io_ex,
                       WsOptions                   ws_options,
                       asio::any_io_executor       data_ex,
                       InstrumentCatalog*          catalog,
                       MultiAccountManager*        accounts,
                       TickStore*                  tick_store,
                       AuthOptions                 auth,
                       std::shared_ptr<audit::Log> audit,
                       ContinuousTable*            continuous)
    : api_(api),
      accounts_(accounts),
      tick_store_(tick_store),
      catalog_(catalog),
      account_router_(api_, accounts_),
      auth_(std::move(auth)),
      audit_(std::move(audit)),
      continuous_(continuous),
      io_ex_(std::move(io_ex)),
      data_ex_(data_ex ? std::move(data_ex) : io_ex_),
      acceptor_(io_ex_),
      ws_(std::make_shared<WebSocketHub>(api_, data_ex_, io_ex_, std::move(ws_options))) {
    ws_->set_catalog(catalog_);
    if (accounts_ != nullptr) {
        accounts_->set_account_removed_handler(
            [this](const std::string& account_id) { drop_account_hub(account_id); });
    }
    setup_routes();
}

HttpServer::~HttpServer() {
    if (accounts_ != nullptr) {
        accounts_->set_account_removed_handler({});
    }
}

std::shared_ptr<WebSocketHub> HttpServer::ensure_ws_hub(const AccountScope& scope) {
    std::lock_guard lock(ws_hubs_mu_);
    auto            it = ws_hubs_.find(scope.id);
    if (it != ws_hubs_.end()) {
        return it->second;
    }
    if (scope.api == api_) {
        ws_hubs_[scope.id] = ws_;
        return ws_;
    }

    auto hub = std::make_shared<WebSocketHub>(scope.api, data_ex_, io_ex_, ws_->options());
    hub->set_catalog(catalog_);
    hub->start();
    ws_hubs_[scope.id] = hub;
    return hub;
}

void HttpServer::drop_account_hub(std::string_view account_id) {
    std::shared_ptr<WebSocketHub> retired;
    {
        std::lock_guard lock(ws_hubs_mu_);
        auto            it = ws_hubs_.find(std::string(account_id));
        if (it == ws_hubs_.end() || it->second == ws_) {
            return;
        }
        retired = std::move(it->second);
        ws_hubs_.erase(it);
    }
    if (retired) {
        retired->stop();
    }
}

AccountScope HttpServer::account_scope(std::string_view account_id) {
    try {
        auto scope  = account_router_.resolve(account_id);
        scope.tasks = &task_registries_[scope.id];
        return scope;
    } catch (const AccountNotFound& e) {
        throw HttpError{http::status::not_found,
                        json{{"error", "account not found"}, {"account", e.account_id}}};
    }
}

TqApi* HttpServer::account_api(std::string_view account_id) {
    return account_scope(account_id).api;
}

namespace {

std::string_view request_path(std::string_view target) {
    const auto pos = target.find('?');
    return pos == std::string_view::npos ? target : target.substr(0, pos);
}

bool secure_equal(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        diff |= static_cast<unsigned char>(lhs[i] ^ rhs[i]);
    }
    return diff == 0;
}

std::string method_name(http::verb method) {
    switch (method) {
    case http::verb::get:
        return "GET";
    case http::verb::post:
        return "POST";
    case http::verb::put:
        return "PUT";
    case http::verb::delete_:
        return "DELETE";
    case http::verb::patch:
        return "PATCH";
    default:
        return "OTHER";
    }
}

std::string peer_string(tcp::socket& socket) {
    boost::system::error_code ec;
    const auto                endpoint = socket.remote_endpoint(ec);
    return ec ? std::string{}
              : endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

bool has_bearer_token(std::string_view header, std::string_view token) {
    constexpr std::string_view prefix = "Bearer ";
    return header.size() == prefix.size() + token.size() && header.starts_with(prefix) &&
           secure_equal(header.substr(prefix.size()), token);
}

} // namespace

bool HttpServer::authorized(const http::request<http::string_body>& req) const {
    if (!auth_.enabled) {
        return true;
    }
    const auto path = request_path(req.target());
    if (auth_.exempt_healthz && path == "/healthz") {
        return true;
    }
    if (auth_.token.empty()) {
        return false;
    }

    if (has_bearer_token(req[http::field::authorization], auth_.token)) {
        return true;
    }
    if (secure_equal(req["X-API-Key"], auth_.token)) {
        return true;
    }
    if (auth_.allow_query_token) {
        const auto query = detail::parse_query(req.target());
        if (secure_equal(query.value("token", std::string{}), auth_.token)) {
            return true;
        }
    }
    return false;
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
    for (const auto& [_, hub] : ws_hubs_) {
        if (hub && hub != ws_) {
            hub->start();
        }
    }
    const auto account_count = accounts_ ? accounts_->account_ids().size() : std::size_t{1};
    SPDLOG_INFO("[HttpServer] listening on {}:{} (ws: {}, accounts: {}, lazy hubs)",
                address,
                port_,
                ws_->options().path,
                account_count);
}

void HttpServer::stop() noexcept {
    if (stopped_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    task_registries_.clear();
    if (ws_) {
        ws_->stop();
    }
    std::vector<std::shared_ptr<WebSocketHub>> hubs;
    {
        std::lock_guard lock(ws_hubs_mu_);
        hubs.reserve(ws_hubs_.size());
        for (const auto& [_, hub] : ws_hubs_) {
            if (hub && hub != ws_) {
                hubs.push_back(hub);
            }
        }
        ws_hubs_.clear();
    }
    for (const auto& hub : hubs) {
        if (hub) {
            hub->stop();
        }
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

            if (!authorized(req)) {
                if (audit_) {
                    audit_->record("ws.auth", "GET", req.target(), 401, peer_string(sock));
                }
                auto res = make_json_response(
                    http::status::unauthorized, json{{"error", "unauthorized"}}, req.version());
                res.set(http::field::www_authenticate, "Bearer");
                res.keep_alive(false);
                co_await asio::dispatch(sock.get_executor(), asio::use_awaitable);
                co_await http::async_write(
                    sock, res, asio::redirect_error(asio::use_awaitable, ec));
                co_return;
            }

            // Hub 的 clients_/订阅表都在 data_ex_ 上；账户解析、懒创建与接管
            // 全部切到 data_ex_，避免与连接/订阅表并发修改。
            const auto query = detail::parse_query(req.target());
            co_await asio::dispatch(data_ex_, asio::use_awaitable);

            auto resolved = account_router_.try_resolve(query.value("account", std::string{}));
            auto hub      = resolved ? ensure_ws_hub(*resolved) : nullptr;
            http::response<http::string_body> res;
            if (target != ws_->options().path) {
                res = make_json_response(
                    http::status::not_found, json{{"error", "not found"}}, req.version());
            } else if (!hub) {
                res = make_json_response(http::status::not_found,
                                         json{{"error", "account not found"},
                                              {"account", query.value("account", std::string{})}},
                                         req.version());
            } else if (hub->full()) {
                res = make_json_response(http::status::service_unavailable,
                                         json{{"error", "too many websocket clients"}},
                                         req.version());
            } else {
                if (audit_) {
                    audit_->record("ws.upgrade", "GET", req.target(), 101, peer_string(sock));
                }
                // 交给对应账户的 WebSocketHub 完成握手并接管 socket。
                co_await hub->accept(std::move(sock), std::move(req));
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

        const auto                        method = req.method();
        const auto                        target = std::string(req.target());
        const auto                        remote = peer_string(sock);
        http::response<http::string_body> res;
        try {
            res = co_await handle(std::move(req));
        } catch (const std::exception& e) {
            res = make_json_response(
                http::status::internal_server_error, json{{"error", e.what()}}, 11);
        }
        if (audit_ && (method != http::verb::get || res.result() == http::status::unauthorized)) {
            audit_->record("http", method_name(method), target, res.result_int(), remote);
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
    if (!authorized(req)) {
        auto res = make_json_response(
            http::status::unauthorized, json{{"error", "unauthorized"}}, version);
        res.set(http::field::www_authenticate, "Bearer");
        co_return res;
    }
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
