#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/gateway/market_data_gateway.h"
#include "tqsdk/gateway/trading_gateway.h"
#include "tqsdk/ipc/ipc_error.h"
#include "tqsdk/ipc/ipc_session.h"
#include "tqsdk/ipc/protocol.h"
#include "tqsdk/ipc/stream_seq.h"
#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {
namespace ipc {

/// IpcClient —— 跑在一条 IpcSession 之上的"帧分发器 + 请求/响应关联器"。
///
/// 一条 gw<->node 连接通常同时承载 行情(md) 与 交易(td) 两类流量。为避免
/// 多个消费者抢同一个 incoming() 通道，这里用**单一后台读循环**消费所有
/// 入站业务帧，按 FrameType 路由给已注册的处理器：
///   - MarketDataPush / GatewayEvent(行情侧)          -> md 处理器
///   - TradePush / CommandAck / GatewayEvent(交易侧)  -> td 处理器
///
/// 同时提供 `Command` 的"发送并等待 CommandAck"能力（按 rid 关联），供
/// RemoteTdGateway 实现 insert_order / cancel_order 的同步语义。
///
/// 生命周期：由上层构造一个已 start()（已 ready）的 IpcSession 传入；
/// IpcClient 启动自己的读循环，直到 session 关闭或 stop()。
class IpcClient {
public:
    /// 行情侧入站处理器：把一帧（MarketDataPush / GatewayEvent）翻译成
    /// MarketDataEvent 并推入网关 output()。
    using MdHandler = std::function<void(ReceivedFrame&&)>;
    /// 交易侧入站处理器：把一帧（TradePush / CommandAck / GatewayEvent）
    /// 翻译成 TradeEvent 并推入网关 event_output()。
    using TdHandler = std::function<void(ReceivedFrame&&)>;
    /// 序号缺口处理器（md 触发 resync，td 触发 replay/reconcile）。
    using GapHandler = std::function<void(const StreamGap&)>;
    /// ResumeAck 处理器（重连后由上层决定是否恢复完成）。
    using ResumeAckHandler = std::function<void(const ResumeAckBody&)>;
    /// 新连接建立（重连成功）时通知上层重放订阅。
    using ReconnectHandler = std::function<void()>;

    struct Metrics {
        uint64_t md_frames          = 0;
        uint64_t td_frames          = 0;
        uint64_t md_gaps            = 0;
        uint64_t td_gaps            = 0;
        uint64_t md_duplicates      = 0;
        uint64_t td_duplicates      = 0;
        uint64_t md_epoch_changes   = 0;
        uint64_t td_epoch_changes   = 0;
        uint64_t md_resume_requests = 0;
        uint64_t td_resume_requests = 0;
    };

    /// 建连工厂：返回一个已完成握手（ready）的新 session。
    /// 重连模式由 IpcClient 在连接断开后调用它重新建连并发送 Resume。
    using SessionFactory = std::function<asio::awaitable<IpcResult<std::unique_ptr<IpcSession>>>()>;

    struct ReconnectOptions {
        bool                      enabled = true;
        std::chrono::milliseconds initial_delay{200};
        std::chrono::milliseconds max_delay{5'000};
    };

    /// 接管一个已就绪的 session（调用方保证 start() 已成功返回）。
    IpcClient(std::unique_ptr<IpcSession> session, asio::any_io_executor ex);

    /// 可重连模式：调用 connect() 建首条连接，start() 启动读循环与重连循环。
    IpcClient(SessionFactory factory, asio::any_io_executor ex);
    IpcClient(SessionFactory factory, asio::any_io_executor ex, ReconnectOptions reconnect);

    ~IpcClient();

    IpcClient(const IpcClient&)            = delete;
    IpcClient& operator=(const IpcClient&) = delete;

    /// 可重连模式的首连：factory 建连并握手。成功返回后调用 start()。
    /// 建连/握手错误通过 IpcFailure 返回，不抛异常。
    asio::awaitable<IpcResult<void>> connect() noexcept;

    /// 启动分发读循环（幂等，只启动一次）。须在设置 md/td 处理器之后调用。
    /// 可重连模式会额外启动连接监督循环。
    void start();

    /// 注册行情/交易侧入站处理器（覆盖式）。应在 start() 前调用。
    void set_md_handler(MdHandler h) { md_handler_ = std::move(h); }
    void set_td_handler(TdHandler h) { td_handler_ = std::move(h); }
    void set_md_gap_handler(GapHandler h) { md_gap_handler_ = std::move(h); }
    void set_td_gap_handler(GapHandler h) { td_gap_handler_ = std::move(h); }
    void set_resume_ack_handler(ResumeAckHandler h) { resume_ack_handler_ = std::move(h); }
    void set_reconnect_handler(ReconnectHandler h) { reconnect_handler_ = std::move(h); }

    /// 发送订阅（md 方向）。成功发出即返回；受理结果由 SubscribeAck 异步到达。
    asio::awaitable<IpcResult<void>> subscribe(const SubscribeBody& body) noexcept;

    /// 发送 Resume（重连后由上层调用）。
    asio::awaitable<IpcResult<void>> send_resume(const ResumeBody& body) noexcept;

    const StreamTracker& md_tracker() const noexcept { return md_tracker_; }
    const StreamTracker& td_tracker() const noexcept { return td_tracker_; }
    Metrics              metrics() const noexcept { return metrics_; }
    nlohmann::json       metrics_json() const;

    /// 发送一条命令并等待对端 CommandAck（按 rid 关联，带超时）。
    /// max_attempts > 1 时，超时后用**同一 rid** 重发；gw 端有命令幂等缓存，
    /// 不会重复提交非幂等报单。会话关闭不重试。
    asio::awaitable<IpcResult<CommandAck>> send_command_and_wait(const CommandBody&        body,
                                                                 std::chrono::milliseconds timeout,
                                                                 int max_attempts = 1) noexcept;

    /// 优雅关闭：发 Goodbye + 关闭 session；唤醒所有 pending 请求。
    void stop() noexcept;

    bool is_open() const noexcept { return session_ && session_->is_open(); }

    uint64_t tx_frames() const noexcept { return session_ ? session_->tx_frames() : 0; }
    uint64_t rx_frames() const noexcept { return session_ ? session_->rx_frames() : 0; }

    FrameEncoding encoding() const noexcept { return encoding_; }

private:
    asio::awaitable<void> read_loop();
    asio::awaitable<void> run_loop();
    asio::awaitable<IpcResult<void>> send_bytes(FrameType type,
                                                const std::vector<uint8_t>& payload) noexcept;
    asio::awaitable<IpcResult<void>> send_resume_if_possible() noexcept;
    void                  notify_disconnected();
    asio::awaitable<void> wait_backoff(std::chrono::milliseconds delay);

    // 等待 ack 的挂起者：用 timer 当一次性"信号量"，close 时 cancel 唤醒。
    struct AckWaiter {
        asio::steady_timer        timer;
        std::optional<CommandAck> result;

        explicit AckWaiter(asio::any_io_executor ex)
            : timer(std::move(ex)) {
            timer.expires_at(std::chrono::steady_clock::time_point::max());
        }
    };

    void complete_ack(const OrderId& rid, const CommandAck& ack) noexcept;

    /// 跟踪一条入站 push 的 seq；duplicate 返回 false（调用方可丢弃）。
    bool
    observe_stream(StreamTracker& tracker, const StreamMeta& meta, GapHandler& handler) noexcept;

    std::unique_ptr<IpcSession> session_;
    asio::any_io_executor       ex_;
    FrameEncoding               encoding_;
    SessionFactory              factory_;
    ReconnectOptions            reconnect_opts_;
    asio::steady_timer          reconnect_timer_;
    // 断线后的旧 session 仍有后台 heartbeat/watchdog 协程在收尾；
    // 保留到 IpcClient 析构，避免其协程访问已释放对象。
    std::vector<std::unique_ptr<IpcSession>> retired_sessions_;

    MdHandler        md_handler_;
    TdHandler        td_handler_;
    GapHandler       md_gap_handler_;
    GapHandler       td_gap_handler_;
    ResumeAckHandler resume_ack_handler_;
    ReconnectHandler reconnect_handler_;

    StreamTracker md_tracker_{StreamKind::Md};
    StreamTracker td_tracker_{StreamKind::Td};
    Metrics       metrics_;

    // rid -> 挂起中的命令等待者
    std::unordered_map<OrderId, AckWaiter*> pending_acks_;
    // 保护 pending_acks_：命令在 send 前登记、读循环/stop 在外部派发，均加锁。
    // 单 io 线程下并非数据竞争必需，但 "先登记后发送" 的时序正确性依赖它，
    // 且能避免将来多线程执行器下出错。
    std::mutex ack_mutex_;

    bool started_{false};
    bool stopped_{false};
};

} // namespace ipc
} // namespace tqsdk
