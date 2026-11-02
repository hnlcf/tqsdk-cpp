#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/gateway/market_data_gateway.h"
#include "tqsdk/gateway/trading_gateway.h"
#include "tqsdk/ipc/ipc_session.h"
#include "tqsdk/ipc/protocol.h"
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

    /// 接管一个已就绪的 session（调用方保证 start() 已成功返回）。
    IpcClient(std::unique_ptr<IpcSession> session, asio::any_io_executor ex);

    ~IpcClient();

    IpcClient(const IpcClient&)            = delete;
    IpcClient& operator=(const IpcClient&) = delete;

    /// 启动分发读循环（幂等，只启动一次）。须在设置 md/td 处理器之后调用。
    void start();

    /// 注册行情/交易侧入站处理器（覆盖式）。应在 start() 前调用。
    void set_md_handler(MdHandler h) { md_handler_ = std::move(h); }
    void set_td_handler(TdHandler h) { td_handler_ = std::move(h); }

    /// 发送订阅（md 方向）。成功发出即返回；受理结果由 SubscribeAck 异步到达。
    asio::awaitable<void> subscribe(const SubscribeBody& body);

    /// 发送一条命令并等待对端 CommandAck（按 rid 关联，带超时）。
    /// 超时 / 会话关闭 抛 IpcError。
    asio::awaitable<CommandAck> send_command_and_wait(const CommandBody&        body,
                                                      std::chrono::milliseconds timeout);

    /// 优雅关闭：发 Goodbye + 关闭 session；唤醒所有 pending 请求。
    void stop() noexcept;

    bool is_open() const noexcept { return session_ && session_->is_open(); }

    uint64_t tx_frames() const noexcept { return session_ ? session_->tx_frames() : 0; }
    uint64_t rx_frames() const noexcept { return session_ ? session_->rx_frames() : 0; }

    FrameEncoding encoding() const noexcept { return encoding_; }

private:
    asio::awaitable<void> read_loop();
    asio::awaitable<void> send_bytes(FrameType type, const std::vector<uint8_t>& payload);

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

    std::unique_ptr<IpcSession> session_;
    asio::any_io_executor       ex_;
    FrameEncoding               encoding_;

    MdHandler md_handler_;
    TdHandler td_handler_;

    // rid -> 挂起中的命令等待者
    std::unordered_map<OrderId, AckWaiter*> pending_acks_;

    bool started_{false};
    bool stopped_{false};
};

} // namespace ipc
} // namespace tqsdk
