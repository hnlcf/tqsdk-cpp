#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/gateway/trading_gateway.h"
#include "tqsdk/ipc/ipc_client.h"
#include "tqsdk/ipc/protocol.h"

namespace tqsdk {
namespace ipc {

/// RemoteTdGateway —— 通过 IPC 把远端 gw 进程的交易网关"代理"成本地网关。
///
/// 实现 TradingGateway 接口：
///   - insert_order() / cancel_order() -> 构造 Command 帧，发往远端并等待
///     CommandAck（按 rid 关联，带超时）；超时抛 IpcError。
///   - 远端推来的 TradePush / GatewayEvent -> 解码为 TradeEvent 推入
///     event_output()，供 TqApi 的 td_loop 消费。
///
/// 依赖一个已就绪的 IpcClient（共享同一条连接）。
///
/// 生命周期两种模式（同 RemoteMdGateway）：
///   - 非拥有：构造传 IpcClient&；
///   - 拥有：构造传 std::shared_ptr<IpcClient>，与共享者共同持有，最后持有者析构时 stop()。
class RemoteTdGateway : public TradingGateway {
public:
    RemoteTdGateway(IpcClient&                client,
                    asio::any_io_executor     ex,
                    std::chrono::milliseconds ack_timeout          = std::chrono::seconds(10),
                    bool                      shfe_market_as_limit = true);
    RemoteTdGateway(std::shared_ptr<IpcClient> client,
                    asio::any_io_executor      ex,
                    std::chrono::milliseconds  ack_timeout          = std::chrono::seconds(10),
                    bool                       shfe_market_as_limit = true);
    ~RemoteTdGateway() override;

    asio::awaitable<void> start() override;
    asio::awaitable<void> stop() override;

    asio::awaitable<CommandAck> insert_order(const InsertOrderReq& req,
                                             const OrderId&        rid) override;
    asio::awaitable<CommandAck> cancel_order(const CancelOrderReq& req) override;

    Channel<TradeEvent>& event_output() override { return ch_; }
    nlohmann::json       metrics() const override;
    bool                 requires_initial_trade_snapshot() const override { return true; }
    bool needs_shfe_price_normalization() const override { return shfe_market_as_limit_; }

    /// 供 IpcClient 调用：把一帧翻译成事件推入 event_output()。
    void on_frame(ReceivedFrame&& frame);

private:
    void push(TradeEvent ev);
    void install_handlers();

    std::shared_ptr<IpcClient> client_owner_; ///< 拥有模式下的共享所有权（可空）
    IpcClient&                 client_;
    asio::any_io_executor      ex_;
    Channel<TradeEvent>        ch_;
    std::chrono::milliseconds  ack_timeout_;
    bool                       shfe_market_as_limit_ = true;
};

} // namespace ipc
} // namespace tqsdk
