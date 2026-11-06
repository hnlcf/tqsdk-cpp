#include "tqsdk/ipc/gateway_server.h"

#include "tqsdk/util/spawn_logged.h"

#include <boost/asio/experimental/awaitable_operators.hpp>

namespace tqsdk {
namespace ipc {

using namespace boost::asio::experimental::awaitable_operators;

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
