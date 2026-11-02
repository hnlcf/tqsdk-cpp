#include "tqsdk/ipc/ipc_client.h"

namespace tqsdk {
namespace ipc {

IpcClient::IpcClient(std::unique_ptr<IpcSession> session, asio::any_io_executor ex)
    : session_(std::move(session)),
      ex_(std::move(ex)),
      encoding_(kDefaultEncoding) {}

IpcClient::~IpcClient() {
    stop();
}

void IpcClient::start() {
    if (started_) {
        return;
    }
    started_ = true;
    co_spawn_logged(
        ex_, [this]() -> asio::awaitable<void> { co_await read_loop(); }, "IpcClient.read");
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

asio::awaitable<CommandAck> IpcClient::send_command_and_wait(const CommandBody&        body,
                                                             std::chrono::milliseconds timeout) {
    if (!session_ || !session_->is_open()) {
        throw IpcError("IpcClient::send_command_and_wait: session closed");
    }

    auto bytes = encode_body(body, encoding_);
    co_await send_bytes(FrameType::Command, bytes);

    // 以 rid 作为关联键（撤单用 order_id）。
    OrderId rid = (body.op == CommandOp::InsertOrder) ? body.rid : body.cancel.order_id;

    AckWaiter w(co_await asio::this_coro::executor);
    pending_acks_[rid] = &w;
    // RAII：退出时一定从表里摘除。
    struct EraseGuard {
        std::unordered_map<OrderId, AckWaiter*>& m;
        OrderId                                  rid;
        ~EraseGuard() { m.erase(rid); }
    } guard{pending_acks_, rid};

    // 超时 timer：同时充当"被 ack 唤醒"的信号（complete_ack 会 cancel 它）。
    w.timer.expires_after(timeout);
    boost::system::error_code ec;
    co_await w.timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

    if (w.result.has_value()) {
        co_return *w.result;
    }
    if (ec == asio::error::operation_aborted) {
        // 被 stop()/关闭唤醒
        throw IpcError("IpcClient::send_command_and_wait: session closed while waiting ack");
    }
    // 超时
    throw IpcError("IpcClient::send_command_and_wait: command ack timeout");
}

void IpcClient::complete_ack(const OrderId& rid, const CommandAck& ack) noexcept {
    auto it = pending_acks_.find(rid);
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
            case FrameType::MarketDataPush:
                if (md_handler_) {
                    // 拷贝一份给 md（frame 需保留给可能的其它处理器；此处帧不共享，move 即可）
                    ReceivedFrame copy{f.type, f.encoding, f.payload};
                    md_handler_(std::move(copy));
                }
                break;

            case FrameType::TradePush:
                if (td_handler_) {
                    td_handler_(std::move(f));
                }
                break;

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
