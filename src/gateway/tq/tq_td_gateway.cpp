#include "tqsdk/gateway/tq/tq_td_gateway.h"

#include "tqsdk/util/log.h"
#include "tqsdk/util/machine_id.h"
#include "tqsdk/util/spawn_logged.h"

#include <openssl/ssl.h>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket/ssl.hpp>

namespace tqsdk {

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
namespace ssl   = asio::ssl;
using boost::asio::use_awaitable;

namespace {

std::string enum_text(auto value) {
    return std::string(magic_enum::enum_name(value));
}

} // namespace

TqTdGateway::TqTdGateway(asio::any_io_executor ex, Options options)
    : ex_(std::move(ex)),
      opts_(std::move(options)),
      ch_(ex_, 4096),
      ssl_ctx_(ssl::context::tls_client),
      write_mutex_(ex_) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
    if (opts_.account_key.empty()) {
        opts_.account_key = opts_.account_id;
    }
}

TqTdGateway::~TqTdGateway() {
    stopped_.store(true, std::memory_order_release);
    ch_.close();
}

TqTdGateway::Url TqTdGateway::parse_url(std::string_view url) {
    Url        parsed;
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return parsed;
    }
    const auto scheme = url.substr(0, scheme_end);
    if (scheme != "wss") {
        SPDLOG_ERROR("[TqTd] only wss:// is supported, got scheme '{}'", scheme);
        return parsed;
    }
    const auto authority_begin = scheme_end + 3;
    const auto path_begin      = url.find('/', authority_begin);
    const auto authority =
        url.substr(authority_begin,
                   path_begin == std::string_view::npos ? std::string_view::npos
                                                        : path_begin - authority_begin);
    if (authority.empty()) {
        return parsed;
    }
    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
        parsed.host = std::string(authority);
    } else {
        parsed.host = std::string(authority.substr(0, colon));
        parsed.port = std::string(authority.substr(colon + 1));
    }
    if (path_begin != std::string_view::npos) {
        parsed.target = std::string(url.substr(path_begin));
    }
    if (parsed.target.empty()) {
        parsed.target = "/";
    }
    return parsed;
}

bool TqTdGateway::is_open() const noexcept {
    return ws_.has_value() && ws_->is_open();
}

asio::awaitable<void> TqTdGateway::start() {
    stopped_.store(false, std::memory_order_release);
    if (opts_.url.empty()) {
        SPDLOG_ERROR("[TqTd] empty td_url");
        co_return;
    }
    co_spawn_logged(
        ex_, [this]() -> asio::awaitable<void> { co_await connect_loop(); }, "TqTd.connect");
    co_return;
}

asio::awaitable<void> TqTdGateway::stop() {
    stopped_.store(true, std::memory_order_release);
    if (ws_) {
        boost::system::error_code ec;
        beast::get_lowest_layer(*ws_).shutdown(tcp_t::socket::shutdown_both, ec);
        beast::get_lowest_layer(*ws_).close(ec);
    }
    if (resolver_) {
        resolver_->cancel();
    }
    ch_.close();
    co_return;
}

asio::awaitable<void> TqTdGateway::connect_loop() {
    auto delay = opts_.reconnect_delay;
    bool first = true;
    while (!stopped_.load(std::memory_order_acquire)) {
        snapshot_ready_ = false;
        pending_diffs_.clear();
        snapshot_ = nlohmann::json::object();
        connected_.store(false, std::memory_order_release);
        if (!first) {
            SPDLOG_INFO("[TqTd] reconnecting in {}ms", delay.count());
            asio::steady_timer timer(ex_);
            timer.expires_after(delay);
            boost::system::error_code ec;
            co_await timer.async_wait(asio::redirect_error(use_awaitable, ec));
            if (stopped_.load(std::memory_order_acquire)) {
                break;
            }
        }
        first = false;
        try {
            co_await connect_once();
        } catch (const std::exception& e) {
            if (!stopped_.load(std::memory_order_acquire)) {
                SPDLOG_WARN("[TqTd] connection ended: {}", e.what());
            }
        }
        ws_.reset();
        connected_.store(false, std::memory_order_release);
        if (stopped_.load(std::memory_order_acquire)) {
            break;
        }
        try {
            co_await ch_.async_send(
                TradeEvent{TradeEvent::Kind::Disconnected, nlohmann::json{}, {}, {}, false});
        } catch (...) {
        }
        delay = std::min(delay * 2, opts_.max_reconnect_delay);
    }
}

asio::awaitable<void> TqTdGateway::connect_once() {
    const auto parsed = parse_url(opts_.url);
    if (parsed.host.empty() || parsed.port.empty()) {
        throw std::runtime_error("invalid TqTd url: " + opts_.url);
    }
    SPDLOG_INFO("[TqTd] connecting {}:{}{} (authorization={})",
                parsed.host,
                parsed.port,
                parsed.target,
                !opts_.authorization.empty());

    resolver_.emplace(ex_);
    auto endpoints = co_await resolver_->async_resolve(parsed.host, parsed.port, use_awaitable);
    tcp_t::socket socket(ex_);
    co_await asio::async_connect(socket, endpoints, use_awaitable);

    ws_.emplace(ssl::stream<tcp_t::socket>(std::move(socket), ssl_ctx_));
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), parsed.host.c_str())) {
        throw std::runtime_error("failed to set TLS SNI");
    }
    ws_->next_layer().set_verify_callback(ssl::host_name_verification(parsed.host));
    ws_->set_option(beast::websocket::stream_base::timeout::suggested(beast::role_type::client));
    beast::websocket::permessage_deflate compression;
    compression.client_enable = true;
    compression.server_enable = true;
    ws_->set_option(compression);
    ws_->read_message_max(8u << 20);
    const auto authorization = opts_.authorization;
    ws_->set_option(beast::websocket::stream_base::decorator(
        [authorization](beast::websocket::request_type& req) {
            req.set(http::field::user_agent,
                    fmt::format("tqsdk-cpp {}", std::string(kClientAppId)));
            if (!authorization.empty()) {
                req.set(http::field::authorization, authorization);
            }
            req.set(http::field::accept, "application/json");
            // 该 OTG 端点要求客户端声明 permessage-deflate；仅设置
            // Beast 的 permessage_deflate 选项不会自动写入握手头。
            req.set("Sec-WebSocket-Extensions", "permessage-deflate; client_max_window_bits");
        }));

    co_await ws_->next_layer().async_handshake(ssl::stream_base::client, use_awaitable);
    const auto host_header =
        parsed.port == "443" ? parsed.host : fmt::format("{}:{}", parsed.host, parsed.port);
    http::response<http::string_body> response;
    boost::system::error_code         handshake_ec;
    co_await ws_->async_handshake(
        response, host_header, parsed.target, asio::redirect_error(use_awaitable, handshake_ec));
    if (handshake_ec) {
        SPDLOG_WARN(
            "[TqTd] handshake rejected: status={} body={}", response.result_int(), response.body());
        throw boost::system::system_error(handshake_ec);
    }
    SPDLOG_DEBUG("[TqTd] handshake extensions={}",
                 std::string(response[http::field::sec_websocket_extensions]));
    SPDLOG_INFO("[TqTd] websocket connected: {}", parsed.host);
    connected_.store(true, std::memory_order_release);

    tq::LoginRequest login;
    login.bid           = opts_.broker_name;
    login.user_name     = opts_.account_id;
    login.password      = opts_.password;
    login.client_app_id = std::string(kClientAppId);
    if (auto mac = util::machine_mac_address(); !mac.empty()) {
        login.client_mac_address = std::move(mac);
    }
    login.broker_id =
        opts_.broker_id.empty() ? std::nullopt : std::optional<std::string>{opts_.broker_id};
    login.front =
        opts_.front_url.empty() ? std::nullopt : std::optional<std::string>{opts_.front_url};
    if (!co_await send_json(tq::serialize(tq::Message{std::move(login)}))) {
        throw std::runtime_error("failed to send TQ login");
    }
    if (!co_await send_json(tq::serialize(tq::Message{tq::ConfirmSettlement{}}))) {
        throw std::runtime_error("failed to send TQ settlement confirmation");
    }
    co_await send_peek();
    co_await read_loop();
}

asio::awaitable<bool> TqTdGateway::send_json(const nlohmann::json& message) {
    if (!ws_.has_value() || !ws_->is_open()) {
        co_return false;
    }
    const auto payload = message.dump();
    try {
        auto guard = co_await AsyncMutex::acquire(write_mutex_);
        if (!ws_.has_value() || !ws_->is_open()) {
            co_return false;
        }
        co_await ws_->async_write(asio::buffer(payload), use_awaitable);
        co_return true;
    } catch (const std::exception& e) {
        SPDLOG_WARN("[TqTd] send failed: {}", e.what());
        co_return false;
    }
}

asio::awaitable<void> TqTdGateway::send_peek() {
    (void)co_await send_json(tq::serialize(tq::Message{tq::PeekMessage{}}));
}

nlohmann::json normalize_td_trade_slice(const nlohmann::json& slice, std::string_view account_key) {
    auto out   = slice;
    auto trade = out.find("trade");
    if (trade == out.end() || !trade->is_object()) {
        return out;
    }
    nlohmann::json normalized = nlohmann::json::object();
    for (auto it = trade->begin(); it != trade->end(); ++it) {
        const auto& value = it.value();
        if (value.is_object() && !value.contains("user")) {
            normalized[std::string(account_key)] = nlohmann::json{{"user", value}};
        } else {
            normalized[std::string(account_key)] = value;
        }
    }
    *trade = std::move(normalized);
    return out;
}

bool td_trade_complete(const nlohmann::json& slice) noexcept {
    if (auto complete = slice.find("trade_more_data");
        complete != slice.end() && complete->is_boolean() && !complete->get<bool>()) {
        return true;
    }
    auto trade = slice.find("trade");
    if (trade == slice.end() || !trade->is_object()) {
        return false;
    }
    for (auto it = trade->begin(); it != trade->end(); ++it) {
        if (!it.value().is_object()) {
            continue;
        }
        const auto& node = it.value();
        if (auto complete = node.find("trade_more_data");
            complete != node.end() && complete->is_boolean() && !complete->get<bool>()) {
            return true;
        }
        if (auto user = node.find("user"); user != node.end() && user->is_object()) {
            if (auto complete = user->find("trade_more_data");
                complete != user->end() && complete->is_boolean() && !complete->get<bool>()) {
                return true;
            }
        }
    }
    return false;
}

nlohmann::json TqTdGateway::normalize_trade_keys(const nlohmann::json& slice) {
    return normalize_td_trade_slice(slice, opts_.account_key);
}

bool TqTdGateway::trade_complete(const nlohmann::json& slice) const {
    return td_trade_complete(slice);
}

void TqTdGateway::merge_json(nlohmann::json& dst, const nlohmann::json& src) {
    if (dst.is_object() && src.is_object()) {
        for (auto it = src.begin(); it != src.end(); ++it) {
            merge_json(dst[it.key()], it.value());
        }
        return;
    }
    dst = src;
}

asio::awaitable<void> TqTdGateway::flush_snapshot() {
    nlohmann::json merged = nlohmann::json::object();
    for (const auto& diff : pending_diffs_) {
        merge_json(merged, diff);
    }
    pending_diffs_.clear();
    snapshot_       = std::move(merged);
    snapshot_ready_ = true;
    co_await ch_.async_send(TradeEvent{TradeEvent::Kind::Snapshot, snapshot_, {}, {}, true});
}

asio::awaitable<void> TqTdGateway::read_loop() {
    while (!stopped_.load(std::memory_order_acquire) && ws_.has_value() && ws_->is_open()) {
        read_buf_.consume(read_buf_.size());
        co_await ws_->async_read(read_buf_, use_awaitable);
        const auto text = beast::buffers_to_string(read_buf_.data());
        read_buf_.consume(read_buf_.size());

        nlohmann::json frame;
        bool           parsed_json = true;
        try {
            frame = nlohmann::json::parse(text);
        } catch (const std::exception& e) {
            SPDLOG_WARN("[TqTd] invalid JSON frame: {}", e.what());
            parsed_json = false;
        }
        if (!parsed_json) {
            co_await send_peek();
            continue;
        }
        auto parsed = tq::parse(frame);
        if (!parsed) {
            SPDLOG_WARN("[TqTd] unsupported frame: {}", parsed.error().detail);
            co_await send_peek();
            continue;
        }

        if (auto* rtn = std::get_if<tq::RtnData>(&*parsed); rtn != nullptr) {
            for (const auto& raw : rtn->data) {
                auto slice = normalize_trade_keys(raw);
                if (!snapshot_ready_) {
                    pending_diffs_.push_back(std::move(slice));
                    if (trade_complete(pending_diffs_.back())) {
                        co_await flush_snapshot();
                    }
                } else {
                    co_await ch_.async_send(
                        TradeEvent{TradeEvent::Kind::Diff, slice, {}, {}, true});
                }
            }
        } else if (auto* brokers = std::get_if<tq::RtnBrokers>(&*parsed); brokers != nullptr) {
            capabilities_ = brokers->capabilities;
        } else if (auto* login = std::get_if<tq::RspLogin>(&*parsed); login != nullptr) {
            const auto code = login->raw.value("code", 0);
            if (code != 0) {
                SPDLOG_WARN("[TqTd] login rejected: {}", login->raw.dump());
            }
        } else if (auto* quotes = std::get_if<tq::RspQuotes>(&*parsed); quotes != nullptr) {
            SPDLOG_DEBUG("[TqTd] rsp_quotes: {}", quotes->raw.dump());
        }
        co_await send_peek();
    }
}

asio::awaitable<CommandAck> TqTdGateway::insert_order(const InsertOrderReq& req,
                                                      const OrderId&        rid) {
    if (!is_open()) {
        co_return CommandAck{false, rid, "TqTd gateway is not connected"};
    }
    std::string exchange   = req.exchange_id;
    std::string instrument = req.instrument_id;
    if (const auto dot = instrument.find('.'); dot != std::string::npos) {
        if (exchange.empty()) {
            exchange = instrument.substr(0, dot);
        }
        instrument = instrument.substr(dot + 1);
    }
    if (exchange.empty() || instrument.empty()) {
        co_return CommandAck{false, rid, "invalid instrument_id"};
    }
    if (req.price_type == PriceType::LIMIT &&
        (!std::isfinite(req.limit_price) || req.limit_price <= 0.0)) {
        co_return CommandAck{false, rid, "LIMIT order requires a valid limit_price"};
    }

    tq::InsertOrder order;
    order.account_key      = opts_.account_key;
    order.user_id          = opts_.account_id;
    order.order_id         = rid;
    order.exchange_id      = exchange;
    order.instrument_id    = instrument;
    order.direction        = req.direction;
    order.offset           = req.offset;
    order.volume           = req.volume;
    order.price_type       = req.price_type;
    order.time_condition   = enum_text(req.time_cond);
    order.volume_condition = enum_text(req.volume_cond);
    if (req.price_type != PriceType::LIMIT && req.time_cond == TimeCondition::GFD) {
        order.time_condition = "IOC";
    }
    if (req.advanced == AdvancedOrder::FAK) {
        order.time_condition   = "IOC";
        order.volume_condition = "ANY";
    } else if (req.advanced == AdvancedOrder::FOK) {
        order.time_condition   = "IOC";
        order.volume_condition = "ALL";
    }
    if (req.price_type == PriceType::LIMIT) {
        order.limit_price = req.limit_price;
    }
    order.offset_leg2 = req.offset_leg2;
    auto payload      = tq::serialize(tq::Message{std::move(order)});
    // BaseOtg 在把交易包发往 OTG 前会移除 account_key；account_key 只用于
    // 本地多账户路由，不是 TD 服务端字段。
    payload.erase("account_key");
    if (!co_await send_json(payload)) {
        co_return CommandAck{false, rid, "failed to send insert_order"};
    }
    co_return CommandAck{true, rid, {}};
}

asio::awaitable<CommandAck> TqTdGateway::cancel_order(const CancelOrderReq& req) {
    if (!is_open()) {
        co_return CommandAck{false, req.order_id, "TqTd gateway is not connected"};
    }
    tq::CancelOrder cancel;
    cancel.account_key = opts_.account_key;
    cancel.user_id     = opts_.account_id;
    cancel.order_id    = req.order_id;
    auto payload       = tq::serialize(tq::Message{std::move(cancel)});
    payload.erase("account_key");
    if (!co_await send_json(payload)) {
        co_return CommandAck{false, req.order_id, "failed to send cancel_order"};
    }
    co_return CommandAck{true, req.order_id, {}};
}

nlohmann::json TqTdGateway::metrics() const {
    return nlohmann::json{{"connected", connected_.load(std::memory_order_acquire)},
                          {"snapshot_ready", snapshot_ready_},
                          {"capabilities", capabilities_}};
}

} // namespace tqsdk
