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
    : GatewayIpcServer(std::move(session), md, td, std::move(ex), Options{}) {}

GatewayIpcServer::GatewayIpcServer(std::unique_ptr<IpcSession> session,
                                   MarketDataGateway*          md,
                                   TradingGateway*             td,
                                   asio::any_io_executor       ex,
                                   Options                     options)
    : GatewayIpcServer(std::move(session), md, td, ex, ex, std::move(options)) {}

GatewayIpcServer::GatewayIpcServer(std::unique_ptr<IpcSession> session,
                                   MarketDataGateway*          md,
                                   TradingGateway*             td,
                                   asio::any_io_executor       ex,
                                   asio::any_io_executor       gateway_ex,
                                   Options                     options)
    : session_(std::move(session)),
      md_(md),
      td_(td),
      ex_(std::move(ex)),
      gateway_ex_(std::move(gateway_ex)),
      md_sink_(std::make_shared<Channel<MarketDataEvent>>(ex_, 2048)),
      td_sink_(std::make_shared<Channel<TradeEvent>>(ex_, 2048)),
      stream_epoch_(make_stream_epoch("gw")),
      md_stream_(StreamKind::Md, stream_epoch_, options.md_replay),
      td_stream_(StreamKind::Td, stream_epoch_, options.td_replay),
      md_stop_timer_(ex_),
      td_stop_timer_(ex_) {}

GatewayIpcServer::~GatewayIpcServer() {
    // 只关闭会话，不 join 后台协程（与 IpcSession 契约一致）。
    close();
}

asio::awaitable<void> GatewayIpcServer::send_md_push(MarketDataPushBody body) {
    body.meta          = md_stream_.next_meta();
    const auto enc     = session_->encoding();
    auto       payload = encode_body(body, enc);
    md_stream_.record(body.meta.seq, FrameType::MarketDataPush, enc, payload);
    co_await session_->send(FrameType::MarketDataPush, ConstSlice{payload});
}

asio::awaitable<void> GatewayIpcServer::send_td_push(TradePushBody body) {
    body.meta          = td_stream_.next_meta();
    const auto enc     = session_->encoding();
    auto       payload = encode_body(body, enc);
    td_stream_.record(body.meta.seq, FrameType::TradePush, enc, payload);
    co_await session_->send(FrameType::TradePush, ConstSlice{payload});
}

asio::awaitable<void> GatewayIpcServer::send_td_snapshot() {
    if (td_ == nullptr) {
        co_return;
    }
    TradeSnapshotBody body;
    body.meta = td_stream_.next_meta();
    // td_->snapshot() 访问共享交易网关状态，必须在 gateway core strand 上执行。
    co_await asio::dispatch(gateway_ex_, asio::use_awaitable);
    body.data = td_->snapshot();
    co_await asio::dispatch(ex_, asio::use_awaitable);
    const auto enc     = session_->encoding();
    auto       payload = encode_body(body, enc);
    td_stream_.record(body.meta.seq, FrameType::TradeSnapshot, enc, payload);
    co_await session_->send(FrameType::TradeSnapshot, ConstSlice{payload});
}

asio::awaitable<void> GatewayIpcServer::handle_resume(const ReceivedFrame& frame) {
    ResumeBody  req;
    bool        decode_ok = true;
    std::string decode_error;
    try {
        req = decode_as<FrameType::Resume>(frame);
    } catch (const std::exception& e) {
        decode_ok    = false;
        decode_error = e.what();
    }

    ResumeAckBody ack;
    ack.epoch       = stream_epoch_;
    ack.md_from_seq = req.md_next_seq;
    ack.td_from_seq = req.td_next_seq;

    std::vector<ReplayFrame> md_replay;
    std::vector<ReplayFrame> td_replay;
    if (!decode_ok) {
        ack.error = std::string("bad Resume: ") + decode_error;
    } else if (req.epoch != stream_epoch_) {
        // epoch 变化说明 gw 重启过，旧 seq 基线全部失效：
        // 不 replay，直接要求 node 重放订阅并接收完整 td 快照。
        ack.error                = "epoch changed";
        ack.md_from_seq          = 0;
        ack.td_from_seq          = 1;
        ack.td_snapshot_required = (td_ != nullptr);
    } else {
        if (req.md_next_seq != 0) {
            ack.md_replay = md_stream_.replay_from(req.md_next_seq, md_replay);
        }
        if (req.td_next_seq != 0) {
            ack.td_replay = td_stream_.replay_from(req.td_next_seq, td_replay);
            if (!ack.td_replay) {
                ack.td_snapshot_required = (td_ != nullptr);
            }
        }
    }

    co_await session_->send_body<FrameType::ResumeAck>(ack);

    for (const auto& f : md_replay) {
        co_await session_->send(f.type, ConstSlice{f.payload});
    }
    for (const auto& f : td_replay) {
        co_await session_->send(f.type, ConstSlice{f.payload});
    }

    if (ack.td_snapshot_required) {
        co_await send_td_snapshot();
    }
    co_return;
}

void GatewayIpcServer::attach_buses(EventBus<MarketDataEvent>* md_bus,
                                    EventBus<TradeEvent>*      td_bus) {
    md_bus_ = md_bus;
    td_bus_ = td_bus;
    if (md_bus_ != nullptr) {
        md_bus_->add_sink(md_sink_);
    }
    if (td_bus_ != nullptr) {
        td_bus_->add_sink(td_sink_);
    }
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
    // 先从扇出总线摘除，再关闭自己的 sink；避免 bus 继续向已析构连接投递。
    if (md_bus_ != nullptr) {
        md_bus_->remove_sink(md_sink_.get());
        md_bus_ = nullptr;
    }
    if (td_bus_ != nullptr) {
        td_bus_->remove_sink(td_sink_.get());
        td_bus_ = nullptr;
    }
    md_sink_->close();
    td_sink_->close();

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
            gateway_ex_,
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

bool GatewayIpcServer::filter_md_for_connection(MarketDataEvent& ev) const {
    if (ev.kind != MarketDataEvent::Kind::Diff && ev.kind != MarketDataEvent::Kind::Kline) {
        return true;
    }
    if (!ev.payload.is_object()) {
        return true;
    }

    bool  any = false;
    auto& j   = ev.payload;

    if (auto qit = j.find("quotes"); qit != j.end() && qit->is_object()) {
        auto& quotes = *qit;
        for (auto it = quotes.begin(); it != quotes.end();) {
            if (symbol_use_.count(it.key()) != 0) {
                any = true;
                ++it;
            } else {
                it = quotes.erase(it);
            }
        }
        if (quotes.empty()) {
            j.erase("quotes");
        }
    }

    if (auto kit = j.find("klines"); kit != j.end() && kit->is_object()) {
        auto& klines = *kit;
        for (auto sit = klines.begin(); sit != klines.end();) {
            if (!sit.value().is_object()) {
                sit = klines.erase(sit);
                continue;
            }
            const std::string symbol = sit.key();
            for (auto dit = sit.value().begin(); dit != sit.value().end();) {
                int64_t dur = 0;
                try {
                    dur = std::stoll(dit.key());
                } catch (...) {
                    dit = sit.value().erase(dit);
                    continue;
                }
                const auto key = std::make_pair(symbol, dur);
                if (std::find(subscribed_klines_.begin(), subscribed_klines_.end(), key) !=
                    subscribed_klines_.end()) {
                    any = true;
                    ++dit;
                } else {
                    dit = sit.value().erase(dit);
                }
            }
            if (sit.value().empty()) {
                sit = klines.erase(sit);
            } else {
                ++sit;
            }
        }
        if (klines.empty()) {
            j.erase("klines");
        }
    }

    // 没有 quotes/klines 字段的未知事件不主动丢弃，保留扩展性。
    if (!ev.payload.contains("quotes") && !ev.payload.contains("klines")) {
        return true;
    }
    return any;
}

// ---------------------------------------------------------------------------
// md_pump：把行情网关事件翻译成 MarketDataPush / GatewayEvent 上推。
// ---------------------------------------------------------------------------
asio::awaitable<void> GatewayIpcServer::md_pump() {
    Channel<MarketDataEvent>& out = (md_bus_ != nullptr) ? *md_sink_ : md_->output();
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

        // 多 node 通过 EventBus 收到同一事件后，各连接只发送自己订阅的部分。
        if (!filter_md_for_connection(*ev)) {
            continue;
        }

        try {
            switch (ev->kind) {
            case MarketDataEvent::Kind::Diff:
            case MarketDataEvent::Kind::Kline: {
                MarketDataPushBody body;
                body.kind = (ev->kind == MarketDataEvent::Kind::Kline) ? MdPushKind::Kline
                                                                       : MdPushKind::Diff;
                body.data = std::move(ev->payload);
                co_await send_md_push(std::move(body));
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
    Channel<TradeEvent>& out = (td_bus_ != nullptr) ? *td_sink_ : td_->event_output();
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
                co_await send_td_push(std::move(body));
                break;
            }
            case TradeEvent::Kind::Snapshot: {
                TradeSnapshotBody body;
                body.meta          = td_stream_.next_meta();
                body.data          = std::move(ev->payload);
                const auto enc     = session_->encoding();
                auto       payload = encode_body(body, enc);
                td_stream_.record(body.meta.seq, FrameType::TradeSnapshot, enc, payload);
                co_await session_->send(FrameType::TradeSnapshot, ConstSlice{payload});
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
        case FrameType::Resume:
            co_await handle_resume(*frame);
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

            // 共享 md 网关状态在 gateway core strand 上。
            co_await asio::dispatch(gateway_ex_, asio::use_awaitable);
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
            co_await asio::dispatch(ex_, asio::use_awaitable);
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

            co_await asio::dispatch(gateway_ex_, asio::use_awaitable);
            try {
                if (!release_syms.empty() || !release_klines.empty()) {
                    co_await md_->release(release_syms, release_klines);
                }
                ack.ok = true;
            } catch (const std::exception& e) {
                ack.ok    = false;
                ack.error = e.what();
            }
            co_await asio::dispatch(ex_, asio::use_awaitable);
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

    std::string cmd_key;
    if (decode_ok && !body.rid.empty()) {
        cmd_key = std::string(1, static_cast<char>(body.op)) + ":" + body.rid;
    }

    if (decode_ok) {
        SPDLOG_DEBUG(
            "[Gw.control] Command op={} rid='{}'", magic_enum::enum_name(body.op), body.rid);
        ack.order_id = body.rid; // 默认回显关联键，失败时也能让消费端正确匹配

        bool cached = false;
        if (!cmd_key.empty()) {
            if (auto it = command_acks_.find(cmd_key); it != command_acks_.end()) {
                ack    = it->second;
                cached = true;
                SPDLOG_DEBUG("[Gw.control] duplicate command rid='{}' -> cached ack", body.rid);
            }
        }

        if (!cached) {
            if (td_ == nullptr) {
                ack.success   = false;
                ack.error_msg = "no trading gateway on this connection";
            } else {
                // 共享交易网关状态在 gateway core strand 上。
                co_await asio::dispatch(gateway_ex_, asio::use_awaitable);
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
                co_await asio::dispatch(ex_, asio::use_awaitable);
            }

            if (!cmd_key.empty()) {
                if (command_acks_.size() >= kMaxCommandAcks && !command_ack_order_.empty()) {
                    command_acks_.erase(command_ack_order_.front());
                    command_ack_order_.pop_front();
                }
                command_acks_[cmd_key] = ack;
                command_ack_order_.push_back(cmd_key);
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
