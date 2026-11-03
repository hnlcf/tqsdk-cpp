#include "tqsdk/ipc/ipc_client.h"

namespace tqsdk {
namespace ipc {

IpcClient::IpcClient(std::unique_ptr<IpcSession> session, asio::any_io_executor ex)
    : session_(std::move(session)),
      ex_(std::move(ex)),
      encoding_(session_ ? session_->encoding() : kDefaultEncoding),
      reconnect_timer_(ex_) {}

IpcClient::IpcClient(SessionFactory factory, asio::any_io_executor ex)
    : IpcClient(std::move(factory), std::move(ex), ReconnectOptions{}) {}

IpcClient::IpcClient(SessionFactory factory, asio::any_io_executor ex, ReconnectOptions reconnect)
    : ex_(std::move(ex)),
      encoding_(kDefaultEncoding),
      factory_(std::move(factory)),
      reconnect_opts_(reconnect),
      reconnect_timer_(ex_) {}

IpcClient::~IpcClient() {
    stop();
}

asio::awaitable<void> IpcClient::connect() {
    if (session_) {
        co_return;
    }
    if (!factory_) {
        throw IpcError("IpcClient::connect: no session factory");
    }
    session_  = co_await factory_();
    encoding_ = session_ ? session_->encoding() : kDefaultEncoding;
}

void IpcClient::start() {
    if (started_) {
        return;
    }
    started_ = true;
    if (factory_) {
        co_spawn_logged(
            ex_, [this]() -> asio::awaitable<void> { co_await run_loop(); }, "IpcClient.run");
    } else {
        co_spawn_logged(
            ex_, [this]() -> asio::awaitable<void> { co_await read_loop(); }, "IpcClient.read");
    }
}

asio::awaitable<void> IpcClient::wait_backoff(std::chrono::milliseconds delay) {
    reconnect_timer_.expires_after(delay);
    boost::system::error_code ec;
    co_await reconnect_timer_.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    co_return;
}

void IpcClient::notify_disconnected() {
    if (stopped_) {
        return;
    }
    GatewayEventBody body{GatewayEventKind::Disconnected, "ipc connection lost"};
    auto             bytes = encode_body(body, encoding_);
    if (md_handler_) {
        md_handler_(ReceivedFrame{FrameType::GatewayEvent, encoding_, bytes});
    }
    if (td_handler_) {
        td_handler_(ReceivedFrame{FrameType::GatewayEvent, encoding_, std::move(bytes)});
    }
}

asio::awaitable<void> IpcClient::send_resume_if_possible() {
    ResumeBody body;
    if (md_tracker_.initialized() && md_tracker_.expected_seq() > 1) {
        body.epoch       = md_tracker_.epoch();
        body.md_next_seq = md_tracker_.expected_seq();
    }
    if (td_tracker_.initialized() && td_tracker_.expected_seq() > 1) {
        if (body.epoch.empty()) {
            body.epoch = td_tracker_.epoch();
        }
        body.td_next_seq = td_tracker_.expected_seq();
    }
    // 即使没有任何历史 seq，也请求一次 td 快照（td_next_seq=1 时 gw 会走
    // reconcile 分支），保证重连后的交易状态与柜台对齐。
    if (body.td_next_seq == 0) {
        body.td_next_seq = 1;
    }
    SPDLOG_INFO("[IpcClient] resume: epoch={} md_next={} td_next={}",
                body.epoch,
                body.md_next_seq,
                body.td_next_seq);
    co_await send_resume(body);
}

asio::awaitable<void> IpcClient::run_loop() {
    auto delay          = reconnect_opts_.initial_delay;
    bool had_connection = session_ != nullptr;

    while (!stopped_) {
        if (!session_) {
            std::string connect_error;
            try {
                session_  = co_await factory_();
                encoding_ = session_ ? session_->encoding() : kDefaultEncoding;
                delay     = reconnect_opts_.initial_delay;
            } catch (const std::exception& e) {
                connect_error = e.what();
            }
            if (!connect_error.empty()) {
                SPDLOG_WARN("[IpcClient] reconnect failed: {}; retry in {}ms",
                            connect_error,
                            delay.count());
                co_await wait_backoff(delay);
                delay = std::min(delay * 2, reconnect_opts_.max_delay);
                continue;
            }

            if (had_connection) {
                if (reconnect_handler_) {
                    reconnect_handler_();
                }
                try {
                    co_await send_resume_if_possible();
                } catch (const std::exception& e) {
                    SPDLOG_WARN("[IpcClient] send Resume failed: {}", e.what());
                }
            }
            had_connection = true;
        }

        co_await read_loop();
        if (session_) {
            session_->close();
            retired_sessions_.push_back(std::move(session_));
        }

        if (stopped_ || !reconnect_opts_.enabled) {
            break;
        }

        notify_disconnected();
        SPDLOG_INFO("[IpcClient] disconnected; reconnect in {}ms", delay.count());
        co_await wait_backoff(delay);
        delay = std::min(delay * 2, reconnect_opts_.max_delay);
    }
    co_return;
}

asio::awaitable<void> IpcClient::send_bytes(FrameType type, const std::vector<uint8_t>& payload) {
    if (!session_ || !session_->is_open()) {
        throw IpcError("IpcClient::send_bytes: session closed");
    }
    co_await session_->send(type, ConstSlice{payload});
}

asio::awaitable<void> IpcClient::subscribe(const SubscribeBody& body) {
    auto bytes = encode_body(body, encoding_);
    co_await send_bytes(FrameType::Subscribe, bytes);
}

asio::awaitable<void> IpcClient::send_resume(const ResumeBody& body) {
    auto bytes = encode_body(body, encoding_);
    co_await send_bytes(FrameType::Resume, bytes);
}

bool IpcClient::observe_stream(StreamTracker&    tracker,
                               const StreamMeta& meta,
                               GapHandler&       handler) noexcept {
    StreamGap gap;
    switch (tracker.observe(meta, &gap)) {
    case StreamTracker::Result::Ignored:
    case StreamTracker::Result::Started:
    case StreamTracker::Result::InOrder:
        return true; // 新流/正常帧：继续投递
    case StreamTracker::Result::EpochChanged:
        if (tracker.kind() == StreamKind::Md) {
            ++metrics_.md_epoch_changes;
        } else {
            ++metrics_.td_epoch_changes;
        }
        return true;
    case StreamTracker::Result::Duplicate:
        if (tracker.kind() == StreamKind::Md) {
            ++metrics_.md_duplicates;
        } else {
            ++metrics_.td_duplicates;
        }
        return false; // 重复帧丢弃
    case StreamTracker::Result::Gap:
        if (tracker.kind() == StreamKind::Md) {
            ++metrics_.md_gaps;
        } else {
            ++metrics_.td_gaps;
        }
        SPDLOG_WARN("[IpcClient] {} seq gap: epoch={} expected={} received={} missing={}",
                    magic_enum::enum_name(tracker.kind()),
                    meta.epoch,
                    gap.expected,
                    gap.received,
                    gap.missing);
        if (handler) {
            handler(gap);
        }
        // 缺口意味着旧 baseline 已断裂：md 走 resync，td 走 replay/快照。
        // 重建后的第一帧会以自身 seq 重新建立基线。
        tracker.reset();
        return true; // 缺口帧仍交付；恢复由 handler 负责
    }
    return true;
}

asio::awaitable<CommandAck> IpcClient::send_command_and_wait(const CommandBody&        body,
                                                             std::chrono::milliseconds timeout,
                                                             int max_attempts) {
    if (!session_ || !session_->is_open()) {
        throw IpcError("IpcClient::send_command_and_wait: session closed");
    }

    // 以 body.rid 作为关联键（调用方必须填写；撤单通常用 order_id）。
    const OrderId rid      = body.rid;
    const int     attempts = std::max(1, max_attempts);

    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (!session_ || !session_->is_open()) {
            throw IpcError("IpcClient::send_command_and_wait: session closed");
        }

        AckWaiter w(ex_);

        // 关键：必须**先登记等待者、再发送**。发送是 co_await，会在本协程挂起期间
        // 让出 io 线程；若对端处理极快，CommandAck 可能在登记之前就被 read_loop
        // 收下并派发，此时若无等待者则会被丢弃，导致本协程永远等不到 ack。
        {
            std::lock_guard<std::mutex> lk(ack_mutex_);
            pending_acks_[rid] = &w;
        }

        // RAII：退出本轮（ack/超时/异常）时一定从表里摘除。
        struct EraseGuard {
            std::unordered_map<OrderId, AckWaiter*>& m;
            std::mutex&                              mu;
            OrderId                                  rid;
            ~EraseGuard() {
                std::lock_guard<std::mutex> lk(mu);
                m.erase(rid);
            }
        } guard{pending_acks_, ack_mutex_, rid};

        auto bytes = encode_body(body, encoding_);
        co_await send_bytes(FrameType::Command, bytes);

        // 超时 timer：同时充当"被 ack 唤醒"的信号（complete_ack 会 cancel 它）。
        w.timer.expires_after(timeout);
        boost::system::error_code ec;
        co_await w.timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

        if (w.result.has_value()) {
            co_return *w.result;
        }
        if (ec == asio::error::operation_aborted) {
            // 被 stop()/关闭唤醒；不重试，避免在已关闭会话上继续发命令。
            throw IpcError("IpcClient::send_command_and_wait: session closed while waiting ack");
        }
        if (attempt < attempts) {
            SPDLOG_WARN("[IpcClient] command rid='{}' ack timeout, retry {}/{}",
                        rid,
                        attempt + 1,
                        attempts);
        }
    }
    throw IpcError("IpcClient::send_command_and_wait: command ack timeout");
}

void IpcClient::complete_ack(const OrderId& rid, const CommandAck& ack) noexcept {
    std::lock_guard<std::mutex> lk(ack_mutex_);
    auto                        it = pending_acks_.find(rid);
    if (it == pending_acks_.end() || it->second == nullptr) {
        return; // 无匹配等待者（可能已超时/重复 ack），忽略
    }
    AckWaiter* w = it->second;
    w->result    = ack;
    w->timer.cancel(); // 唤醒挂起的 send_command_and_wait
}

asio::awaitable<void> IpcClient::read_loop() {
    try {
        while (session_ && session_->is_open()) {
            auto opt = co_await session_->incoming().async_receive();
            if (!opt.has_value()) {
                break; // session 关闭
            }
            ReceivedFrame f = std::move(*opt);

            switch (f.type) {
            case FrameType::MarketDataPush: {
                bool deliver = true;
                try {
                    auto body = decode_as<FrameType::MarketDataPush>(f);
                    ++metrics_.md_frames;
                    deliver = observe_stream(md_tracker_, body.meta, md_gap_handler_);
                } catch (const std::exception& e) {
                    SPDLOG_WARN("IpcClient: bad MarketDataPush: {}", e.what());
                }
                if (deliver && md_handler_) {
                    ReceivedFrame copy{f.type, f.encoding, f.payload};
                    md_handler_(std::move(copy));
                }
                break;
            }

            case FrameType::TradePush: {
                bool deliver = true;
                try {
                    auto body = decode_as<FrameType::TradePush>(f);
                    ++metrics_.td_frames;
                    deliver = observe_stream(td_tracker_, body.meta, td_gap_handler_);
                } catch (const std::exception& e) {
                    SPDLOG_WARN("IpcClient: bad TradePush: {}", e.what());
                }
                if (deliver && td_handler_) {
                    td_handler_(std::move(f));
                }
                break;
            }

            case FrameType::TradeSnapshot: {
                // 快照是权威截面：直接以它的 seq 重建 td 基线，并忽略 gap 回调。
                bool deliver = true;
                try {
                    auto body = decode_as<FrameType::TradeSnapshot>(f);
                    ++metrics_.td_frames;
                    td_tracker_.reset();
                    td_tracker_.observe(body.meta);
                } catch (const std::exception& e) {
                    SPDLOG_WARN("IpcClient: bad TradeSnapshot: {}", e.what());
                    deliver = false;
                }
                if (deliver && td_handler_) {
                    ReceivedFrame copy{f.type, f.encoding, f.payload};
                    td_handler_(std::move(copy));
                }
                break;
            }

            case FrameType::ResumeAck: {
                try {
                    auto ack = decode_as<FrameType::ResumeAck>(f);
                    ++metrics_.md_resume_requests;
                    ++metrics_.td_resume_requests;
                    if (!ack.md_replay) {
                        md_tracker_.reset();
                        // 重连时上层会通过 reconnect_handler 重放订阅；
                        // 只有确实请求过 md 历史时才在此重复触发 resync。
                        if (ack.md_from_seq != 0 && md_gap_handler_) {
                            md_gap_handler_(
                                StreamGap{StreamKind::Md, ack.epoch, ack.md_from_seq, 0, 0});
                        }
                    }
                    if (!ack.td_replay) {
                        td_tracker_.reset();
                        if (ack.td_from_seq > 1 && !ack.td_snapshot_required && td_gap_handler_) {
                            td_gap_handler_(
                                StreamGap{StreamKind::Td, ack.epoch, ack.td_from_seq, 0, 0});
                        }
                    }
                    if (resume_ack_handler_) {
                        resume_ack_handler_(ack);
                    }
                } catch (const std::exception& e) {
                    SPDLOG_WARN("IpcClient: bad ResumeAck: {}", e.what());
                }
                break;
            }

            case FrameType::CommandAck: {
                // 直接完成等待者，同时把 ack 事件也交给 td（TqApi 可能想感知）。
                try {
                    auto body = decode_as<FrameType::CommandAck>(f);
                    complete_ack(body.ack.order_id, body.ack);
                } catch (const std::exception& e) {
                    SPDLOG_WARN("IpcClient: bad CommandAck: {}", e.what());
                }
                if (td_handler_) {
                    ReceivedFrame copy{f.type, f.encoding, f.payload};
                    td_handler_(std::move(copy));
                }
                break;
            }

            case FrameType::GatewayEvent:
                // 连接状态事件同时影响 md 与 td 两侧。
                if (md_handler_) {
                    ReceivedFrame copy{f.type, f.encoding, f.payload};
                    md_handler_(std::move(copy));
                }
                if (td_handler_) {
                    ReceivedFrame copy{f.type, f.encoding, f.payload};
                    td_handler_(std::move(copy));
                }
                break;

            case FrameType::SubscribeAck:
                // 订阅受理结果：一期仅记日志（订阅错误交由上层按需处理）。
                try {
                    auto ack = decode_as<FrameType::SubscribeAck>(f);
                    if (!ack.ok) {
                        SPDLOG_WARN("IpcClient: Subscribe rejected: {}", ack.error);
                    }
                } catch (const std::exception& e) {
                    SPDLOG_WARN("IpcClient: bad SubscribeAck: {}", e.what());
                }
                break;

            default:
                SPDLOG_DEBUG("IpcClient: ignore frame type {}", static_cast<int>(f.type));
                break;
            }
        }
    } catch (const boost::system::system_error& e) {
        if (e.code() != asio::error::operation_aborted) {
            SPDLOG_WARN("IpcClient: read error: {}", e.what());
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("IpcClient: read exception: {}", e.what());
    }

    // 读循环结束：唤醒所有仍在等待 ack 的请求。
    for (auto& [rid, w] : pending_acks_) {
        if (w != nullptr) {
            w->timer.cancel();
        }
    }
    co_return;
}

void IpcClient::stop() noexcept {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    reconnect_timer_.cancel();

    // 唤醒所有 pending 请求（session 关闭后 read_loop 也会做一次）。
    for (auto& [rid, w] : pending_acks_) {
        if (w != nullptr) {
            w->timer.cancel();
        }
    }

    if (session_) {
        session_->close();
    }
}

} // namespace ipc
} // namespace tqsdk
