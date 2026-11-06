#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/gateway/market_data_gateway.h"
#include "tqsdk/gateway/trading_gateway.h"
#include "tqsdk/ipc/command_ack_cache.h"
#include "tqsdk/ipc/event_bus.h"
#include "tqsdk/ipc/ipc_session.h"
#include "tqsdk/ipc/protocol.h"
#include "tqsdk/ipc/stream_seq.h"

namespace tqsdk {
namespace ipc {

/// GatewayIpcServer —— 接入进程（gw）侧：把**真实网关**桥接成一条 IPC 连接。
///
/// 这是 RemoteMdGateway / RemoteTdGateway（消费端）在生产端的对偶：
///   - 消费端（策略/node）把 output() 里的 MarketDataEvent 当成本地网关事件；
///   - 生产端（gw）把真实网关 output()/event_output() 里的事件编码成 IPC 帧上推。
///
/// 数据流：
///   MarketDataGateway::output()      --md_pump-->  MarketDataPush / GatewayEvent   ==> node
///   TradingGateway::event_output()   --td_pump-->  TradePush / GatewayEvent        ==> node
///   真实网关的 subscribe()/insert_order()/...  <--control_loop--  Subscribe/Command <= node
///
/// 帧语义约定（与 protocol.h 对齐）：
///   - md Diff/Kline  -> MarketDataPush{kind=Diff|Kline, data=payload}
///   - md/gw Error    -> GatewayEvent{kind=Error, text=error_msg}
///   - md Disconnected-> GatewayEvent{kind=Disconnected, text}
///   - td Diff        -> TradePush{kind=Diff, data=payload}
///   - td Disconnected-> GatewayEvent{kind=Disconnected}
///   - td Error       -> GatewayEvent{kind=Error}
///   - td Ack         -> **不上推**（Ack 已作为对应 Command 的 CommandAck 同步返回，
///                        再上推会与 CommandAck 语义重复；消费端 td_loop 对 Ack 也不处理）
///   - Subscribe      -> 调用网关 subscribe/subscribe_kline，回 SubscribeAck
///   - Command        -> 调用网关 insert_order/cancel_order，回 CommandAck（按 body.rid 关联）
///
/// 生命周期与线程：
///   - 所有方法在 session 所属 io_context 线程上调用（单线程模型）。
///   - 构造要求 session 已完成 start()（已 ready）；否则由调用方负责。
///   - close() 幂等：停止 pump（关闭事件源）并关闭 session。
///   - 网关指针不拥有（由上层持有），可为 nullptr 表示"该类流量不存在"。
///
/// 易忽视细节：
///   - md/td 两个 pump 与心跳、控制应答会并发写同一 socket，IpcSession::send
///     内部以 AsyncMutex 串行化写，故本类可放心并发 send。
///   - 网关 output() 关闭（nullopt）即视为该侧结束，pump 退出；不会关闭 session，
///     因为另一侧可能仍在工作。整条连接的关闭由 close()/对端 Goodbye/断线驱动。
class GatewayIpcServer : public std::enable_shared_from_this<GatewayIpcServer> {
public:
    /// replay buffer 容量（md 可较小，td 需要覆盖断线窗口）。
    struct Options {
        ReplayConfig md_replay{4096, 16u << 20};
        ReplayConfig td_replay{16384, 64u << 20};
    };

    /// 接管一条已就绪的 session；md/td 可为 nullptr。
    GatewayIpcServer(std::unique_ptr<IpcSession> session,
                     MarketDataGateway*          md,
                     TradingGateway*             td,
                     asio::any_io_executor       ex);

    GatewayIpcServer(std::unique_ptr<IpcSession> session,
                     MarketDataGateway*          md,
                     TradingGateway*             td,
                     asio::any_io_executor       ex,
                     Options                     options);

    /// 每连接独立 strand 版本：
    ///   - ex         ：本连接的 IpcSession/pump/control 所在 strand；
    ///   - gateway_ex ：共享 md/td 网关（subscribe/release/snapshot）所在 core strand。
    GatewayIpcServer(std::unique_ptr<IpcSession> session,
                     MarketDataGateway*          md,
                     TradingGateway*             td,
                     asio::any_io_executor       ex,
                     asio::any_io_executor       gateway_ex,
                     Options                     options);

    ~GatewayIpcServer();

    GatewayIpcServer(const GatewayIpcServer&)            = delete;
    GatewayIpcServer& operator=(const GatewayIpcServer&) = delete;

    /// 挂接扇出总线（多 node 共享同一 gw 网关时必须使用）。
    /// 须在 start() 前调用；bus 生命周期由调用方持有，通常覆盖整个进程。
    /// 未挂接时退化为直接消费网关 output()（单连接测试/兼容路径）。
    void attach_buses(EventBus<MarketDataEvent>* md_bus, EventBus<TradeEvent>* td_bus);

    /// 启动 md/td pump 与 control 循环（后台协程）。幂等。
    ///
    /// 自持语义：每个后台协程都持有一份 shared_from_this()，因此本对象在
    /// 所有后台循环退出前不会被销毁——调用方 `start()` 之后可以安全地丢弃
    /// 自己的 shared_ptr 引用（close 通过持有引用触发或由循环退出自然结束）。
    void start();

    /// 停止并关闭会话（幂等）。会关闭 session（唤醒读循环/看门狗），
    /// 进而让后台 pump/control 循环以 nullopt 退出，最终释放自持引用。
    void close() noexcept;

    bool is_open() const noexcept { return session_ && session_->is_open(); }

    /// 本连接 IpcSession/pump/control 所在 executor（用于从其他 strand 安全 close）。
    asio::any_io_executor executor() const noexcept { return ex_; }

    IpcSession& session() noexcept { return *session_; }

    const std::string& stream_epoch() const noexcept { return stream_epoch_; }
    uint64_t           md_next_seq() const noexcept { return md_stream_.next_seq(); }
    uint64_t           td_next_seq() const noexcept { return td_stream_.next_seq(); }

private:
    asio::awaitable<void> md_pump();
    asio::awaitable<void> td_pump();
    asio::awaitable<void> control_loop();

    /// 挂起到 close()：各自独立的停止 timer，避免互相取消对方挂起的等待
    /// （steady_timer 上重新 expires_at 会 cancel 该 timer 的其它挂起 wait）。
    asio::awaitable<void> wait_stop(asio::steady_timer& t);

    /// 处理一条 Subscribe：驱动网关订阅并回 ack。
    asio::awaitable<void> handle_subscribe(const ReceivedFrame& frame);
    /// 处理一条 Command：驱动网关下单/撤单并回 CommandAck。
    asio::awaitable<void> handle_command(const ReceivedFrame& frame);
    /// 处理一条 Resume：按 epoch/seq 决定 replay 或要求 resync/reconcile。
    asio::awaitable<void> handle_resume(const ReceivedFrame& frame);

    /// 给 md/td push 分配 seq、记录 replay buffer、发送。
    asio::awaitable<void> send_md_push(MarketDataPushBody body);
    asio::awaitable<void> send_td_push(TradePushBody body);
    /// 发送交易全量快照（td stream 也占一个 seq）。
    asio::awaitable<void> send_td_snapshot();

    /// 按本连接的订阅集合裁剪 md diff（quotes/klines）。
    /// 返回 false 表示该事件对本连接没有任何内容，可直接丢弃。
    bool filter_md_for_connection(MarketDataEvent& ev) const;

    std::unique_ptr<IpcSession> session_;
    MarketDataGateway*          md_;
    TradingGateway*             td_;
    asio::any_io_executor       ex_;         // 本连接 strand
    asio::any_io_executor       gateway_ex_; // 共享网关 core strand

    // 多 node 扇出：attach_buses 后 md_pump/td_pump 消费自己的 sink，
    // 由 EventBus 把共享网关事件 post 到本连接 strand。
    EventBus<MarketDataEvent>*                md_bus_{nullptr};
    EventBus<TradeEvent>*                     td_bus_{nullptr};
    std::shared_ptr<Channel<MarketDataEvent>> md_sink_;
    std::shared_ptr<Channel<TradeEvent>>      td_sink_;

    std::string    stream_epoch_;
    OutboundStream md_stream_;
    OutboundStream td_stream_;

    // 命令幂等缓存；key = "<op>:<rid>"，control_loop 单协程访问。
    CommandAckCache command_acks_{4096};

    // 本连接累计订阅的合约与 K 线 (symbol,duration_ns)。close() 时一次性 release 给
    // 网关，使网关按引用计数释放（多 node 共享同一 md 网关时的正确退订语义）。
    //
    // symbol_use_ 统计"本连接内该 symbol 的使用次数"：显式 subscribe +1，
    // 每个 kline 依赖再 +1。这样显式退订行情但 K 线仍在时不会提前释放合约行情。
    std::vector<std::string> subscribed_symbols_; // 计数 >0 的 symbol（每 symbol 一次）
    std::vector<std::pair<std::string, int64_t>> subscribed_klines_;
    std::map<std::string, int>                   symbol_use_;
    std::set<std::string>                        subscribed_kline_keys_; // "sym|dur"

    // 停止信号：pump 在"等待网关事件"与"等待停止"之间做 race，close() 时
    // cancel 本 timer 即可让阻塞在网关 output 上的 pump 立即退出，避免
    // "关闭连接后 pump 仍挂起、网关通道被析构"的悬垂访问。
    // md/td 各用一个 timer，互不干扰。
    asio::steady_timer md_stop_timer_;
    asio::steady_timer td_stop_timer_;

    bool started_{false};
    bool closed_{false};
};

} // namespace ipc
} // namespace tqsdk
