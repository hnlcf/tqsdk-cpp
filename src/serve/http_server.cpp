#include "tqsdk/serve/http_server.h"
#include "tqsdk/trading/holiday_calendar.h"

#include "tqsdk/domain/sentinel.h"
#include "tqsdk/serve/json_convert.h"
#include "tqsdk/util/json_value.h"
#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {
namespace serve {

HttpServer::HttpServer(TqApi*                api,
                       asio::any_io_executor ex,
                       WsOptions             ws_options,
                       InstrumentCatalog*    catalog,
                       MultiAccountManager*  accounts,
                       TickStore*            tick_store)
    : HttpServer(api, ex, std::move(ws_options), ex, catalog, accounts, tick_store) {}

HttpServer::HttpServer(TqApi*                api,
                       asio::any_io_executor io_ex,
                       WsOptions             ws_options,
                       asio::any_io_executor data_ex,
                       InstrumentCatalog*    catalog,
                       MultiAccountManager*  accounts,
                       TickStore*            tick_store)
    : api_(api),
      accounts_(accounts),
      tick_store_(tick_store),
      catalog_(catalog),
      io_ex_(std::move(io_ex)),
      data_ex_(data_ex ? std::move(data_ex) : io_ex_),
      acceptor_(io_ex_),
      ws_(std::make_shared<WebSocketHub>(api_, data_ex_, io_ex_, std::move(ws_options))) {
    ws_->set_catalog(catalog_);
    ws_default_ = ws_;
    if (accounts_ != nullptr) {
        const WsOptions hub_options = ws_->options();
        const auto      default_id  = accounts_->default_account_id();
        for (const auto& id : accounts_->account_ids()) {
            auto account = accounts_->find_account(id);
            if (!account)
                continue;
            std::shared_ptr<WebSocketHub> hub;
            if (account.get() == api_) {
                hub = ws_;
            } else {
                hub = std::make_shared<WebSocketHub>(account.get(), data_ex_, io_ex_, hub_options);
                hub->set_catalog(catalog_);
            }
            ws_hubs_[id] = hub;
            if (id == default_id)
                ws_default_ = hub;
        }
    } else {
        ws_hubs_.emplace("", ws_);
    }
    setup_routes();
}

std::shared_ptr<WebSocketHub> HttpServer::ws_hub_for(std::string_view account_id) const {
    if (account_id.empty())
        return ws_default_;
    auto it = ws_hubs_.find(std::string(account_id));
    return it == ws_hubs_.end() ? nullptr : it->second;
}

TqApi* HttpServer::account_api(std::string_view account_id) const {
    if (accounts_ == nullptr) {
        return api_;
    }
    std::string id{account_id};
    if (id.empty())
        id = accounts_->default_account_id();
    auto api = accounts_->find_account(id);
    if (!api) {
        throw HttpError{http::status::not_found,
                        json{{"error", "account not found"}, {"account", id}}};
    }
    return api.get();
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
        if (hub && hub != ws_)
            hub->start();
    }
    SPDLOG_INFO("[HttpServer] listening on {}:{} (ws: {}, accounts: {})",
                address,
                port_,
                ws_->options().path,
                ws_hubs_.size());
}

void HttpServer::stop() noexcept {
    if (stopped_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    target_tasks_.clear();
    order_tasks_.clear();
    task_accounts_.clear();
    if (ws_) {
        ws_->stop();
    }
    for (const auto& [_, hub] : ws_hubs_) {
        if (hub && hub != ws_)
            hub->stop();
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
            const auto query = detail::parse_query(req.target());
            auto       hub   = ws_default_;
            if (query.contains("account")) {
                const auto account_id = query["account"].get<std::string>();
                hub                   = ws_hub_for(account_id);
            }
            co_await asio::dispatch(data_ex_, asio::use_awaitable);

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
