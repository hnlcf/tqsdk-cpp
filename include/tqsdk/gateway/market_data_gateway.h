#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/channel/channel.h"

namespace tqsdk {

/// 行情网关向上游(TqApi)推送的统一事件。
/// 网关不负责解析/落库，只把"与原协议同构的 JSON diff"塞进 payload，
/// 由 TqApi 的后台协程统一交给 DiffMergeEngine 合并。
struct MarketDataEvent {
    enum class Kind : uint8_t {
        Diff,         // payload 是一条行情 diff（quotes / klines 等）
        Kline,        // payload 是 kline diff（保留，便于上游区分）
        Disconnected, // 与服务器断开，payload 可忽略
        Error,        // 网关内部错误，error_msg 有效
    } kind;

    nlohmann::json payload;   // 与原协议同构的 JSON
    std::string    error_msg; // Kind::Error 时有效
};

/// MarketDataGateway 是行情网关的抽象接口，所有行情网关都必须继承自此类。
///
/// 契约：
///   - start()/stop() 的生命周期由 TqApi 控制；stop() 应关闭 output() 通道，
///     使 TqApi 的 md_loop 以 nullopt 正常退出。
///   - 网关只负责"把 diff 推入 output()"，不解析、不落库，合并统一在 TqApi 做。
///   - 阶段 0~3 是单线程 io_context，网关不得跨线程直接操作 Channel 内部状态。
class MarketDataGateway {
public:
    virtual ~MarketDataGateway() = default;

    virtual asio::awaitable<void> start() = 0;
    virtual asio::awaitable<void> stop()  = 0;

    virtual asio::awaitable<void> subscribe(const std::vector<std::string>& symbols) = 0;

    // 订阅 K 线（序列）。duration_ns 为纳秒周期，view_width 为期望根数。
    // 默认空实现：不支持 K 线的网关（如 mock/简单网关）无需覆写即可继续工作；
    // 需要 K 线的网关（如 TqMdGateway）覆写并通过 output() 推送 kline diff。
    virtual asio::awaitable<void>
    subscribe_kline(const std::string& symbol, int64_t duration_ns, int view_width) {
        (void)symbol;
        (void)duration_ns;
        (void)view_width;
        co_return;
    }

    // 网关内部将收到的行情 diff 推入此通道；TqApi 的后台 md_loop 从此消费。
    virtual Channel<MarketDataEvent>& output() = 0;
};
} // namespace tqsdk
