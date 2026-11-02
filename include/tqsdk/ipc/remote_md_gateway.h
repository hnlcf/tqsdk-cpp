#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/gateway/market_data_gateway.h"
#include "tqsdk/ipc/ipc_client.h"
#include "tqsdk/ipc/protocol.h"

namespace tqsdk {
namespace ipc {

/// RemoteMdGateway —— 通过 IPC 把远端 gw 进程的行情网关"代理"成本地网关。
///
/// 实现 MarketDataGateway 接口，因此 TqApi 可像使用进程内网关一样使用它：
///   - subscribe() / subscribe_kline() -> 发 Subscribe 帧（由 IpcClient 转发）
///   - 远端推来的 MarketDataPush / GatewayEvent -> 解码为 MarketDataEvent
///     推入本地 output() 通道，供 TqApi 的 md_loop 消费。
///
/// 依赖一个已就绪的 IpcClient（共享同一条连接，允许多个网关共存）。
class RemoteMdGateway : public MarketDataGateway {
public:
    RemoteMdGateway(IpcClient& client, asio::any_io_executor ex);

    asio::awaitable<void> start() override;
    asio::awaitable<void> stop() override;

    asio::awaitable<void> subscribe(const std::vector<std::string>& symbols) override;
    asio::awaitable<void> subscribe_kline(const std::string& symbol,
                                          int64_t            duration_ns,
                                          int                view_width) override;

    Channel<MarketDataEvent>& output() override { return ch_; }

    /// 供 IpcClient 调用：把一帧翻译成事件推入 output()。
    void on_frame(ReceivedFrame&& frame);

private:
    void push(MarketDataEvent ev);

    IpcClient&              client_;
    asio::any_io_executor   ex_;
    Channel<MarketDataEvent> ch_;
};

} // namespace ipc
} // namespace tqsdk
