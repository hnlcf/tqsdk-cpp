#include "tqsdk/ipc/gateway_server.h"

#include "tqsdk/util/spawn_logged.h"

#include <boost/asio/experimental/awaitable_operators.hpp>

namespace tqsdk {
namespace ipc {

using namespace boost::asio::experimental::awaitable_operators;

GatewayIpcServer::GatewayIpcServer(std::unique_ptr<IpcSession> session,
                                   MarketDataGateway*          md,
                                   TradingGateway*             td,
                                   asio::any_io_executor       ex)
    : session_(std::move(session)),
      md_(md),
      td_(td),
      ex_(std::move(ex)),
      md_stop_timer_(ex_),
      td_stop_timer_(ex_) {}

GatewayIpcServer::~GatewayIpcServer() {
    // 只关闭会话，不 join 后台协程（与 IpcSession 契约一致）。
    close();
}

void GatewayIpcServer::start() {
    if (started_ || closed_) {
        return;
    }
    started_ = true;

    // 每个后台协程持有一份 self，确保它们在运行期间本对象不被销毁。
    auto self = shared_from_this();

    if (md_ != nullptr) {
        co_spawn_logged(
            ex_, [self]() -> asio::awaitable<void> { co_await self->md_pump(); }, "Gw.md_pump");
    }
    if (td_ != nullptr) {
        co_spawn_logged(
            ex_, [self]() -> asio::awaitable<void> { co_await self->td_pump(); }, "Gw.td_pump");
    }
    co_spawn_logged(
        ex_, [self]() -> asio::awaitable<void> { co_await self->control_loop(); }, "Gw.control");
}

void GatewayIpcServer::close() noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    // 唤醒阻塞在网关 output 上的 pump（wait_stop 分支）。
    md_stop_timer_.cancel();
    td_stop_timer_.cancel();
    if (session_) {
        session_->close();
    }
    // 释放本连接在 md 网关上的订阅（引用计数退订）。按值捕获订阅快照，
    // 避免 release 协程运行期间本对象被销毁导致的悬垂访问；md_ 由上层长期持有。
    if (md_ != nullptr && (!subscribed_symbols_.empty() || !subscribed_klines_.empty())) {
        auto* md     = md_;
        auto  syms   = std::move(subscribed_symbols_);
        auto  klines = std::move(subscribed_klines_);
        SPDLOG_TRACE("[Gw] close: releasing {} symbols, {} klines", syms.size(), klines.size());
        subscribed_symbols_.clear();
        subscribed_klines_.clear();
        symbol_use_.clear();
        subscribed_kline_keys_.clear();
        co_spawn_logged(
            ex_,
            [md, syms = std::move(syms), klines = std::move(klines)]() -> asio::awaitable<void> {
                try {
                    co_await md->release(syms, klines);
                } catch (const std::exception& e) {
                    SPDLOG_WARN("[Gw] release subscriptions failed: {}", e.what());
                }
            },
            "Gw.release");
    }
}

asio::awaitable<void> GatewayIpcServer::wait_stop(asio::steady_timer& t) {
    if (closed_) {
        co_return;
    }
    // 设一个极远的到期点等待 close() 取消（每个 pump 用自己的 timer，
    // 避免 expires_at 互相取消对方挂起的 wait）。
    t.expires_at(std::chrono::steady_clock::time_point::max());
    boost::system::error_code ec;
    co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    co_return;
}

// ---------------------------------------------------------------------------
// md_pump：把行情网关事件翻译成 MarketDataPush / GatewayEvent 上推。
// ---------------------------------------------------------------------------
asio::awaitable<void> GatewayIpcServer::md_pump() {
    auto& out = md_->output();
    while (!closed_) {
        // race：网关事件 vs 停止信号。若停止先到，接收方被取消并退出循环。
        std::variant<std::optional<MarketDataEvent>, std::monostate> raced;
        try {
            raced = co_await (out.async_receive() || wait_stop(md_stop_timer_));
        } catch (const boost::system::system_error& e) {
            if (e.code() == asio::error::operation_aborted) {
                co_return;
            }
            throw;
        }
        if (std::holds_alternative<std::monostate>(raced)) {
            co_return; // 停止信号获胜
        }
        auto ev = std::move(std::get<0>(raced));
        if (!ev.has_value()) {
            // 网关关闭了事件源：md 側结束，但不连带关闭整条连接。
            SPDLOG_DEBUG("[Gw.md_pump] md output closed");
            co_return;
        }

        try {
            switch (ev->kind) {
            case MarketDataEvent::Kind::Diff:
            case MarketDataEvent::Kind::Kline: {
                MarketDataPushBody body;
                body.kind = (ev->kind == MarketDataEvent::Kind::Kline) ? MdPushKind::Kline
                                                                       : MdPushKind::Diff;
                body.data = std::move(ev->payload);
                co_await session_->send_body<FrameType::MarketDataPush>(body);
                break;
            }
            case MarketDataEvent::Kind::Disconnected:
                co_await session_->send_body<FrameType::GatewayEvent>(
                    GatewayEventBody{GatewayEventKind::Disconnected, std::string{}});
                break;
            case MarketDataEvent::Kind::Error:
                co_await session_->send_body<FrameType::GatewayEvent>(
                    GatewayEventBody{GatewayEventKind::Error, ev->error_msg});
                break;
            }
        } catch (const std::exception& e) {
            SPDLOG_WARN("[Gw.md_pump] send failed: {}", e.what());
            co_return;
        }
    }
    co_return;
}

// ---------------------------------------------------------------------------
// td_pump：把交易网关事件翻译成 TradePush / GatewayEvent 上推。
// ---------------------------------------------------------------------------
asio::awaitable<void> GatewayIpcServer::td_pump() {
    auto& out = td_->event_output();
    while (!closed_) {
        std::variant<std::optional<TradeEvent>, std::monostate> raced;
        try {
            raced = co_await (out.async_receive() || wait_stop(td_stop_timer_));
        } catch (const boost::system::system_error& e) {
            if (e.code() == asio::error::operation_aborted) {
                co_return;
            }
            throw;
        }
        if (std::holds_alternative<std::monostate>(raced)) {
            co_return; // 停止信号获胜
        }
        auto ev = std::move(std::get<0>(raced));
        if (!ev.has_value()) {
            SPDLOG_DEBUG("[Gw.td_pump] td output closed");
            co_return;
        }

        try {
            switch (ev->kind) {
            case TradeEvent::Kind::Diff: {
                TradePushBody body;
                body.kind = TradePushKind::Diff;
                body.data = std::move(ev->payload);
                co_await session_->send_body<FrameType::TradePush>(body);
                break;
            }
            case TradeEvent::Kind::Ack:
                // 命令 ack 已在 handle_command 中同步返回 CommandAck，这里不重复上推。
                break;
            case TradeEvent::Kind::Disconnected:
                co_await session_->send_body<FrameType::GatewayEvent>(
                    GatewayEventBody{GatewayEventKind::Disconnected, std::string{}});
                break;
            case TradeEvent::Kind::Error:
                co_await session_->send_body<FrameType::GatewayEvent>(
                    GatewayEventBody{GatewayEventKind::Error, ev->error_msg});
                break;
            }
        } catch (const std::exception& e) {
            SPDLOG_WARN("[Gw.td_pump] send failed: {}", e.what());
            co_return;
        }
    }
    co_return;
}

// ---------------------------------------------------------------------------
// control_loop：消费 node 下发的 Subscribe / Command。
// ---------------------------------------------------------------------------
asio::awaitable<void> GatewayIpcServer::control_loop() {
    auto& in = session_->incoming();
    while (!closed_) {
        std::optional<ReceivedFrame> frame;
        try {
            frame = co_await in.async_receive();
        } catch (const boost::system::system_error& e) {
            if (e.code() == asio::error::operation_aborted) {
                co_return;
            }
            throw;
        }
        if (!frame.has_value()) {
            SPDLOG_DEBUG("[Gw.control] incoming closed");
            co_return;
        }
        SPDLOG_DEBUG("[Gw.control] recv frame type={}", static_cast<int>(frame->type));

        switch (frame->type) {
        case FrameType::Subscribe:
            co_await handle_subscribe(*frame);
            break;
        case FrameType::Command:
            co_await handle_command(*frame);
            break;
        default:
            SPDLOG_WARN("[Gw.control] unexpected frame type {}", static_cast<int>(frame->type));
            break;
        }
    }
    co_return;
}

asio::awaitable<void> GatewayIpcServer::handle_subscribe(const ReceivedFrame& frame) {
    SubscribeAckBody ack{false, ""};
    SubscribeBody    body;
    bool             decode_ok = true;
    try {
        body = decode_as<FrameType::Subscribe>(frame);
    } catch (const std::exception& e) {
        decode_ok = false;
        ack.error = std::string("bad Subscribe: ") + e.what();
    }

    // co_await 不能放在 catch 内：解码成功后在此处理。
    if (decode_ok) {
        if (md_ == nullptr) {
            ack.ok    = false;
            ack.error = "no market-data gateway on this connection";
        } else if (!body.release) {
            // 订阅：symbol_use_ 做连接内引用计数，显式 symbols 与 kline 依赖各 +1；
            // 只有 0->1 的 symbol 才向上游 subscribe，与 release 严格对称。
            std::vector<std::string> new_syms;
            std::set<std::string>    seen_explicit;

            auto use_symbol = [&](const std::string& s) {
                if (symbol_use_[s]++ == 0) {
                    new_syms.push_back(s);
                    subscribed_symbols_.push_back(s);
                }
            };

            for (const auto& s : body.symbols) {
                if (!seen_explicit.insert(s).second) {
                    continue;
                }
                use_symbol(s);
            }

            std::vector<KlineSub> new_klines;
            for (const auto& k : body.klines) {
                const std::string key = k.symbol + "|" + std::to_string(k.duration_ns);
                if (!subscribed_kline_keys_.insert(key).second) {
                    continue; // 同连接重复订阅同一 kline：不重复加计数
                }
                use_symbol(k.symbol);
                new_klines.push_back(k);
                subscribed_klines_.emplace_back(k.symbol, k.duration_ns);
            }

            try {
                if (!new_syms.empty()) {
                    co_await md_->subscribe(new_syms);
                }
                for (const auto& k : new_klines) {
                    co_await md_->subscribe_kline(k.symbol, k.duration_ns, k.view_width);
                }
                ack.ok = true;
            } catch (const std::exception& e) {
                ack.ok    = false;
                ack.error = e.what();
            }
        } else {
            // 退订：显式 symbols 与 kline 依赖各 -1；归零才向上游 release。
            std::vector<std::string>                     release_syms;
            std::vector<std::pair<std::string, int64_t>> release_klines;

            auto release_symbol_once = [&](const std::string& s) {
                auto it = symbol_use_.find(s);
                if (it == symbol_use_.end()) {
                    return;
                }
                if (--it->second <= 0) {
                    symbol_use_.erase(it);
                    std::erase(subscribed_symbols_, s);
                    release_syms.push_back(s);
                }
            };

            std::set<std::string> seen_explicit;
            for (const auto& s : body.symbols) {
                if (!seen_explicit.insert(s).second) {
                    continue;
                }
                release_symbol_once(s);
            }
            for (const auto& k : body.klines) {
                const std::string key = k.symbol + "|" + std::to_string(k.duration_ns);
                if (subscribed_kline_keys_.erase(key) == 0) {
                    continue; // 未订阅过该 kline：忽略，避免重复扣减
                }
                std::erase(subscribed_klines_, std::make_pair(k.symbol, k.duration_ns));
                release_klines.emplace_back(k.symbol, k.duration_ns);
                release_symbol_once(k.symbol);
            }

            try {
                if (!release_syms.empty() || !release_klines.empty()) {
                    co_await md_->release(release_syms, release_klines);
                }
                ack.ok = true;
            } catch (const std::exception& e) {
                ack.ok    = false;
                ack.error = e.what();
            }
        }
    }

    try {
        co_await session_->send_body<FrameType::SubscribeAck>(ack);
    } catch (const std::exception& e) {
        SPDLOG_WARN("[Gw.control] send SubscribeAck failed: {}", e.what());
    }
    co_return;
}

asio::awaitable<void> GatewayIpcServer::handle_command(const ReceivedFrame& frame) {
    CommandAck  ack{false, {}, ""};
    CommandBody body;
    bool        decode_ok = true;
    try {
        body = decode_as<FrameType::Command>(frame);
    } catch (const std::exception& e) {
        decode_ok     = false;
        ack.success   = false;
        ack.error_msg = std::string("bad Command: ") + e.what();
    }

    if (decode_ok) {
        SPDLOG_DEBUG(
            "[Gw.control] Command op={} rid='{}'", magic_enum::enum_name(body.op), body.rid);
        ack.order_id = body.rid; // 默认回显关联键，失败时也能让消费端正确匹配
        if (td_ == nullptr) {
            ack.success   = false;
            ack.error_msg = "no trading gateway on this connection";
        } else {
            try {
                switch (body.op) {
                case CommandOp::InsertOrder:
                    ack = co_await td_->insert_order(body.insert, body.rid);
                    break;
                case CommandOp::CancelOrder:
                    ack = co_await td_->cancel_order(body.cancel);
                    break;
                }
                // 保证关联键至少回显（某些网关成功时可能不回填 order_id）。
                if (ack.order_id.empty()) {
                    ack.order_id = body.rid;
                }
            } catch (const std::exception& e) {
                ack.success   = false;
                ack.order_id  = body.rid;
                ack.error_msg = e.what();
            }
        }
    }

    try {
        co_await session_->send_body<FrameType::CommandAck>(CommandAckBody{ack});
    } catch (const std::exception& e) {
        SPDLOG_WARN("[Gw.control] send CommandAck failed: {}", e.what());
    }
    co_return;
}

} // namespace ipc
} // namespace tqsdk
