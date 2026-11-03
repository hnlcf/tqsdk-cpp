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
///
/// 生命周期有两种模式：
///   - 非拥有：构造传 IpcClient&（测试/调用方自行管理 client）。
///   - 拥有：构造传 std::shared_ptr<IpcClient>，本网关与共享者共同持有 client，
///     最后一个持有者析构时 client 自动 stop()。配置化接入（TqApiFactory）用此模式。
class RemoteMdGateway : public MarketDataGateway {
public:
    RemoteMdGateway(IpcClient& client, asio::any_io_executor ex);
    RemoteMdGateway(std::shared_ptr<IpcClient> client, asio::any_io_executor ex);
    ~RemoteMdGateway() override;

    asio::awaitable<void> start() override;
    asio::awaitable<void> stop() override;

    asio::awaitable<void> subscribe(const std::vector<std::string>& symbols) override;
    asio::awaitable<void>
    subscribe_kline(const std::string& symbol, int64_t duration_ns, int view_width) override;
    asio::awaitable<void>
    release(const std::vector<std::string>&                     symbols,
            const std::vector<std::pair<std::string, int64_t>>& klines) override;

    Channel<MarketDataEvent>& output() override { return ch_; }

    /// 供 IpcClient 调用：把一帧翻译成事件推入 output()。
    void on_frame(ReceivedFrame&& frame);

    /// md 发现 seq gap / Resume 失败时重放当前有效订阅（触发全量首帧）。
    asio::awaitable<void> resync();

private:
    void push(MarketDataEvent ev);
    void install_handlers();

    std::shared_ptr<IpcClient> client_owner_; ///< 拥有模式下的共享所有权（可空）
    IpcClient&                 client_;
    asio::any_io_executor      ex_;
    Channel<MarketDataEvent>   ch_;

    // node 侧当前有效订阅，供断线 resync 重放。
    std::set<std::string>                          symbols_;
    std::map<std::pair<std::string, int64_t>, int> klines_; // key -> view_width
};

} // namespace ipc
} // namespace tqsdk
