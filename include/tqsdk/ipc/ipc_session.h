#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/channel/channel.h"
#include "tqsdk/ipc/frame.h"
#include "tqsdk/ipc/ipc_channle.h"
#include "tqsdk/ipc/protocol.h"
#include "tqsdk/util/async_mutex.h"

namespace tqsdk {
namespace ipc {

/// IpcSession —— 在一个 IIpcChannel 之上提供"有条理的全双工连接"。
///
/// 职责（控制面）：
///   1. 握手状态机：双方一启动即互发 Hello，收到对方 Hello 回 HelloAck；
///      查到 HelloAck(ok) 或收到对方 Hello 即进入 Ready。握手超时则失败关闭。
///   2. 心跳：周期性发送 Heartbeat；收到任何帧都刷新 last_rx；
///      超过 heartbeat_timeout 未收到任何帧则判定断线并关闭。
///   3. 收发分离：内部读循环 async_recv -> 分派；业务帧投递到 incoming() channel。
///   4. 优雅关闭：send Goodbye + close channel；幂等。
///
/// 不负责：seq 编号的具体承载（body 内字段由上层决定）、业务语义。
/// 暴露 tx_frames()/rx_frames() 计数便于观测。
///
/// 线程模型：所有方法必须在 channel 所属 io_context 线程上调用（asio 约束）。
class IpcSession {
public:
    struct Options {
        IpcRole                   local_role = IpcRole::Gateway;
        std::string               local_name;       ///< Hello.name
        std::string               peer_name_expect; ///< 可空：期望对端名（仅日志）
        std::chrono::milliseconds handshake_timeout{std::chrono::seconds(5)};
        std::chrono::milliseconds heartbeat_interval{std::chrono::seconds(5)};
        std::chrono::milliseconds heartbeat_timeout{std::chrono::seconds(15)};
        std::size_t               incoming_capacity = 0; ///< 0=无界
        FrameEncoding             encoding          = kDefaultEncoding;
    };

    /// 接管一个已建立的 channel。构造后须调用 start()。
    IpcSession(std::unique_ptr<IIpcChannel> channel, asio::any_io_executor ex, Options opts);

    ~IpcSession();

    IpcSession(const IpcSession&)            = delete;
    IpcSession& operator=(const IpcSession&) = delete;

    /// 发起握手并启动读循环/心跳。返回时握手已成功；失败抛异常（IpcError / system_error）。
    /// 读循环与心跳在后台协程中运行，直到 close() 或连接断开。
    asio::awaitable<void> start();

    /// 发送一个业务帧。编码由 Options.encoding 决定。
    asio::awaitable<void> send(FrameType type, ConstSlice payload);

    /// 便捷：编码 body 并发送（encoding 用 Options.encoding）。
    template <FrameType T, typename Body>
    asio::awaitable<void> send_body(const Body& body) {
        auto bytes = encode_body(body, opts_.encoding);
        co_await send(T, ConstSlice{bytes});
    }

    /// 业务帧入口（非控制帧）。close() 后 async_receive 返回 nullopt。
    Channel<ReceivedFrame>& incoming() noexcept { return incoming_; }

    /// 优雅/强制关闭。幂等，可在任意时刻调用。
    void close() noexcept;

    bool          is_open() const noexcept { return !closed_; }
    bool          is_ready() const noexcept { return ready_; }
    FrameEncoding encoding() const noexcept { return opts_.encoding; }

    uint64_t tx_frames() const noexcept { return tx_frames_; }
    uint64_t rx_frames() const noexcept { return rx_frames_; }

private:
    asio::awaitable<void> read_loop();
    asio::awaitable<void> heartbeat_loop();
    asio::awaitable<void> watchdog_loop();

    /// 处理一帧；返回 false 表示连接应终止（如收到 Goodbye 或协议致命错误）。
    asio::awaitable<bool> dispatch_frame(ReceivedFrame frame);

    void mark_ready() noexcept;
    void on_peer_closed() noexcept;

    std::unique_ptr<IIpcChannel> channel_;
    asio::any_io_executor        ex_;
    Options                      opts_;

    Channel<ReceivedFrame> incoming_;

    // 握手就绪信号：ready 时 cancel（触发 operation_aborted 唤醒 start）。
    asio::steady_timer ready_timer_;
    // 入站活跃计时：收到任意帧时设置为 now+heartbeat_timeout，超时判定断线。
    asio::steady_timer rx_watchdog_;
    // 心跳发送节拍器（成员化以便 close 时 cancel，避免析构后回调）。
    asio::steady_timer heartbeat_timer_;

    bool     ready_{false};
    bool     closed_{false};
    bool     started_{false};
    uint64_t tx_frames_{0};
    uint64_t rx_frames_{0};

    // 仍在运行的后台协程数；start() 失败时会等待其归零，避免悬空 this。
    std::atomic<int> loops_running_{0};

    // 串行化所有出站写：底层 socket 不允许并发 async_write（会破坏帧序列）。
    // 心跳循环、业务 send、控制帧应答可能并发，必须经过本互斥。
    AsyncMutex write_mutex_;
};

} // namespace ipc
} // namespace tqsdk
