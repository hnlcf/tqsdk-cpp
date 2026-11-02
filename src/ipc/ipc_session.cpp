#include "tqsdk/ipc/ipc_session.h"

#include "tqsdk/types.h"
#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {
namespace ipc {

IpcSession::IpcSession(std::unique_ptr<IIpcChannel> channel, asio::any_io_executor ex, Options opts)
    : channel_(std::move(channel)),
      ex_(std::move(ex)),
      opts_(std::move(opts)),
      incoming_(ex_, opts_.incoming_capacity),
      ready_timer_(ex_),
      rx_watchdog_(ex_),
      heartbeat_timer_(ex_),
      write_mutex_(ex_) {}

IpcSession::~IpcSession() {
    // 不在此 join 协程；但必须已 close，避免挂起的 waiter 触发 Channel 析构断言。
    close();
}

asio::awaitable<void> IpcSession::start() {
    if (started_) {
        throw IpcError("IpcSession::start called twice");
    }
    started_ = true;

    // 1) 主动发 Hello
    co_await send_body<FrameType::Hello>(
        HelloBody{kIpcProtocolVersion, opts_.local_role, opts_.local_name});

    // 2) 启动读循环 / 心跳 / 看门狗（后台运行）
    loops_running_ = true;
    co_spawn_logged(
        ex_, [this]() -> asio::awaitable<void> { co_await read_loop(); }, "IpcSession.read");
    co_spawn_logged(
        ex_,
        [this]() -> asio::awaitable<void> { co_await heartbeat_loop(); },
        "IpcSession.heartbeat");
    co_spawn_logged(
        ex_,
        [this]() -> asio::awaitable<void> { co_await watchdog_loop(); },
        "IpcSession.watchdog");

    // 看门狗：启动即 arm，收到任意帧会重置
    rx_watchdog_.expires_after(opts_.heartbeat_timeout);

    // 3) 等待握手就绪或超时
    ready_timer_.expires_after(opts_.handshake_timeout);
    boost::system::error_code ec;
    co_await ready_timer_.async_wait(asio::redirect_error(asio::use_awaitable, ec));

    if (!ready_) {
        // 超时或握手中途被关闭
        close();
        throw IpcError("IpcSession: handshake timeout / failed");
    }
    co_return;
}

asio::awaitable<void> IpcSession::send(FrameType type, ConstSlice payload) {
    if (closed_) {
        throw IpcError("IpcSession::send on closed session");
    }
    // 串行化写：socket 不允许并发 async_write。锁内重新检查 closed_（可能在排队期间关闭）。
    auto guard = co_await AsyncMutex::acquire(write_mutex_);
    if (closed_) {
        throw IpcError("IpcSession::send on closed session");
    }
    co_await channel_->async_send(type, opts_.encoding, payload);
    ++tx_frames_;
}

asio::awaitable<bool> IpcSession::dispatch_frame(ReceivedFrame frame) {
    ++rx_frames_;
    // 任意帧刷新看门狗
    if (!closed_) {
        rx_watchdog_.expires_after(opts_.heartbeat_timeout);
    }

    switch (frame.type) {
    case FrameType::Hello: {
        // 校验 version / role（decode 在 try 内，co_await 移到 try 外）
        HelloBody hello;
        bool      decode_ok = true;
        try {
            hello = decode_body<HelloBody>(frame);
        } catch (const std::exception& e) {
            decode_ok = false;
            SPDLOG_WARN("IpcSession[{}]: bad Hello: {}", opts_.local_name, e.what());
        }
        if (!decode_ok) {
            co_await send_body<FrameType::HelloAck>(HelloAckBody{false, std::string("bad hello")});
            co_return false;
        }
        const bool version_ok = (hello.version == kIpcProtocolVersion);
        const bool role_ok    = (hello.role != opts_.local_role);
        if (!version_ok || !role_ok) {
            std::string err = !version_ok ? "version mismatch" : "role conflict";
            SPDLOG_WARN("IpcSession[{}]: reject Hello ({}): peer v{} role {}",
                        opts_.local_name,
                        err,
                        hello.version,
                        magic_enum::enum_name(hello.role));
            co_await send_body<FrameType::HelloAck>(HelloAckBody{false, err});
            co_return false;
        }
        co_await send_body<FrameType::HelloAck>(HelloAckBody{true, ""});
        SPDLOG_INFO("IpcSession[{}]: peer '{}' ({}) handshake ok",
                    opts_.local_name,
                    hello.name,
                    magic_enum::enum_name(hello.role));
        mark_ready();
        co_return true;
    }
    case FrameType::HelloAck: {
        HelloAckBody ack;
        try {
            ack = decode_body<HelloAckBody>(frame);
        } catch (const std::exception& e) {
            SPDLOG_WARN("IpcSession[{}]: bad HelloAck: {}", opts_.local_name, e.what());
            co_return false;
        }
        if (!ack.ok) {
            SPDLOG_WARN("IpcSession[{}]: HelloAck rejected: {}", opts_.local_name, ack.error);
            co_return false;
        }
        mark_ready();
        co_return true;
    }
    case FrameType::Heartbeat:
        co_await send_body<FrameType::HeartbeatAck>(HeartbeatBody{});
        co_return true;
    case FrameType::HeartbeatAck:
        co_return true; // 看门狗已在上面刷新
    case FrameType::Goodbye: {
        GoodbyeBody gb;
        try {
            gb = decode_body<GoodbyeBody>(frame);
        } catch (...) {
            // 允许空/非法 body
        }
        SPDLOG_INFO("IpcSession[{}]: peer said goodbye: {}", opts_.local_name, gb.reason);
        co_return false; // 终止
    }
    default:
        // 业务帧：投递到 incoming()；关闭后投递失败即忽略
        try {
            co_await incoming_.async_send(std::move(frame));
        } catch (const boost::system::system_error& e) {
            if (e.code() == asio::error::operation_aborted) {
                co_return false;
            }
            throw;
        }
        co_return true;
    }
}

asio::awaitable<void> IpcSession::read_loop() {
    try {
        while (!closed_) {
            auto f = co_await channel_->async_recv();
            if (!f.has_value()) {
                SPDLOG_INFO("IpcSession[{}]: peer closed channel", opts_.local_name);
                break;
            }
            const bool keep = co_await dispatch_frame(std::move(*f));
            if (!keep) {
                break;
            }
        }
    } catch (const boost::system::system_error& e) {
        if (e.code() != asio::error::operation_aborted) {
            SPDLOG_WARN("IpcSession[{}]: read error: {}", opts_.local_name, e.what());
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("IpcSession[{}]: read exception: {}", opts_.local_name, e.what());
    }
    on_peer_closed();
    co_return;
}

asio::awaitable<void> IpcSession::heartbeat_loop() {
    while (!closed_) {
        if (ready_) {
            try {
                co_await send_body<FrameType::Heartbeat>(HeartbeatBody{});
            } catch (...) {
                // 发送失败：连接多半已坏，read_loop/看门狗会处理
                co_return;
            }
        }
        heartbeat_timer_.expires_after(opts_.heartbeat_interval);
        boost::system::error_code ec;
        co_await heartbeat_timer_.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        if (ec == asio::error::operation_aborted) {
            co_return; // close() 唤醒
        }
    }
    co_return;
}

asio::awaitable<void> IpcSession::watchdog_loop() {
    while (!closed_) {
        boost::system::error_code ec;
        co_await rx_watchdog_.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        if (ec == asio::error::operation_aborted) {
            // 被 close() cancel；若确已 closed 则退出
            if (closed_) {
                co_return;
            }
            continue;
        }
        if (closed_) {
            co_return;
        }
        // 若 deadline 被收帧重置到未来，则继续等；否则判超时
        if (rx_watchdog_.expiry() > std::chrono::steady_clock::now()) {
            continue;
        }
        SPDLOG_WARN("IpcSession[{}]: heartbeat timeout, closing", opts_.local_name);
        close();
        co_return;
    }
    co_return;
}

void IpcSession::mark_ready() noexcept {
    if (ready_) {
        return;
    }
    ready_ = true;
    ready_timer_.cancel(); // 唤醒 start()
}

void IpcSession::on_peer_closed() noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    channel_->close();
    ready_timer_.cancel();
    rx_watchdog_.cancel();
    heartbeat_timer_.cancel();
    incoming_.close();
    loops_running_ = false;
}

void IpcSession::close() noexcept {
    if (closed_) {
        // 仍确保底层资源释放
        if (channel_) {
            channel_->close();
        }
        return;
    }
    closed_ = true;
    if (channel_) {
        channel_->close();
    }
    ready_timer_.cancel();
    rx_watchdog_.cancel();
    heartbeat_timer_.cancel();
    incoming_.close();
    loops_running_ = false;
}

} // namespace ipc
} // namespace tqsdk
