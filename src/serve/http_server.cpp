#include "tqsdk/serve/http_server.h"

#include "tqsdk/domain/enum_util.h"
#include "tqsdk/domain/sentinel.h"
#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {
namespace serve {

namespace {

using json = nlohmann::json;

// NaN 无法被 JSON 序列化（nlohmann 会抛/输出 null 取决于配置）；统一映射为 null，
// 让"未知值"在 HTTP 层表现为 null 而非崩溃。
json jnum(double v) {
    return std::isfinite(v) ? json(v) : json(nullptr);
}

json enum_str(auto&& e) {
    // 注意：不能写 json{std::string(...)}——花括号初始化会被当成单元素数组，
    // 必须用括号构造得到字符串。
    return json(std::string(magic_enum::enum_name(e)));
}

json quote_to_json(const Quote& q) {
    json j;
    j["instrument_id"]   = q.instrument_id;
    j["datetime"]        = q.datetime;
    j["last_price"]      = jnum(q.last_price);
    j["pre_close"]       = jnum(q.pre_close);
    j["pre_settlement"]  = jnum(q.pre_settlement);
    j["open"]            = jnum(q.open);
    j["highest"]         = jnum(q.highest);
    j["lowest"]          = jnum(q.lowest);
    j["volume"]          = q.volume;
    j["turnover"]        = jnum(q.turnover);
    j["open_interest"]   = jnum(q.open_interest);
    j["close"]           = jnum(q.close);
    j["upper_limit"]     = jnum(q.upper_limit);
    j["lower_limit"]     = jnum(q.lower_limit);
    j["volume_multiple"] = q.volume_multiple;
    j["price_tick"]      = jnum(q.price_tick);
    j["bid_price1"]      = jnum(q.bid_price1);
    j["ask_price1"]      = jnum(q.ask_price1);
    j["bid_volume1"]     = q.bid_volume1;
    j["ask_volume1"]     = q.ask_volume1;
    return j;
}

json kline_to_json(const KlineStream& s) {
    json j;
    j["symbol"]      = s.instrumnet_id;
    j["duration_ns"] = s.duration_ns;
    j["bars"]        = json::array();
    for (const auto& b : s.bars) {
        j["bars"].push_back(json{{"datetime", b.datetime},
                                 {"open", jnum(b.open)},
                                 {"high", jnum(b.high)},
                                 {"low", jnum(b.low)},
                                 {"close", jnum(b.close)},
                                 {"volume", b.volume},
                                 {"open_oi", b.open_oi},
                                 {"close_oi", b.close_oi}});
    }
    return j;
}

json account_to_json(const Account& a) {
    json j;
    j["currency"]        = enum_str(a.currency);
    j["balance"]         = jnum(a.balance);
    j["available"]       = jnum(a.available);
    j["margin"]          = jnum(a.margin);
    j["frozen_margin"]   = jnum(a.frozen_margin);
    j["risk_ratio"]      = jnum(a.risk_ratio);
    j["pre_balance"]     = jnum(a.pre_balance);
    j["close_profit"]    = jnum(a.close_profit);
    j["commission"]      = jnum(a.commission);
    j["float_profit"]    = jnum(a.float_profit);
    j["position_profit"] = jnum(a.position_profit);
    return j;
}

json order_to_json(const Order& o) {
    json j;
    j["order_id"]      = o.order_id;
    j["instrument_id"] = o.instrument_id;
    j["exchange_id"]   = o.exchange_id;
    j["direction"]     = enum_str(o.direction);
    j["offset"]        = enum_str(o.offset);
    j["volume_orign"]  = o.volume_orign;
    j["volume_left"]   = o.volume_left;
    j["price_type"]    = enum_str(o.price_type);
    j["limit_price"]   = jnum(o.limit_price);
    j["trade_price"]   = jnum(o.trade_price);
    j["status"]        = enum_str(o.status);
    j["last_msg"]      = o.last_msg;
    j["is_dead"]       = o.is_dead;
    j["is_error"]      = o.is_error;
    return j;
}

json trade_to_json(const Trade& t) {
    json j;
    j["trade_id"]        = t.trade_id;
    j["order_id"]        = t.order_id;
    j["instrument_id"]   = t.instrument_id;
    j["direction"]       = enum_str(t.direction);
    j["offset"]          = enum_str(t.offset);
    j["volume"]          = t.volume;
    j["price"]           = jnum(t.price);
    j["trade_date_time"] = t.trade_date_time;
    return j;
}

json position_to_json(const Position& p) {
    json j;
    j["instrument_id"]        = p.instrument_id;
    j["volume_long"]          = p.volume_long;
    j["volume_short"]         = p.volume_short;
    j["volume_long_today"]    = p.volume_long_today;
    j["volume_short_today"]   = p.volume_short_today;
    j["open_price_long"]      = jnum(p.open_price_long);
    j["open_price_short"]     = jnum(p.open_price_short);
    j["position_price_long"]  = jnum(p.position_price_long);
    j["position_price_short"] = jnum(p.position_price_short);
    j["last_price"]           = jnum(p.last_price);
    j["float_profit"]         = jnum(p.float_profit);
    j["position_profit"]      = jnum(p.position_profit);
    j["margin"]               = jnum(p.margin);
    return j;
}

// 路由路径解析 / URL 解码 / 参数提取均已下沉到 serve/router.h。

} // namespace

HttpServer::HttpServer(TqApi* api, asio::any_io_executor ex)
    : api_(api),
      ex_(std::move(ex)),
      acceptor_(ex_) {
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
        ex_, [this]() -> asio::awaitable<void> { co_await accept_loop(); }, "Http.accept");
    SPDLOG_INFO("[HttpServer] listening on {}:{}", address, port_);
}

void HttpServer::stop() noexcept {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    boost::system::error_code ec;
    auto                      _ = acceptor_.close(ec);
}

asio::awaitable<void> HttpServer::accept_loop() {
    for (;;) {
        boost::system::error_code ec;
        tcp::socket               sock(co_await asio::this_coro::executor);
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
            ex_,
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

        // keep-alive 支持：根据请求决定是否关闭。
        const bool close_conn = req.need_eof();

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
