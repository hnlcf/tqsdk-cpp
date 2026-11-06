#include "tqsdk/ipc/gateway_server.h"

#include "tqsdk/util/spawn_logged.h"

#include <boost/asio/experimental/awaitable_operators.hpp>

namespace tqsdk {
namespace ipc {

using namespace boost::asio::experimental::awaitable_operators;

GatewayIpcServer::GatewayIpcServer(std::unique_ptr<IpcSession> session,
                                   MarketDataGateway*          md,
                                   TradingGateway*             td,
                                   asio::any_io_executor       ex)
    : GatewayIpcServer(std::move(session), md, td, std::move(ex), Options{}) {}

GatewayIpcServer::GatewayIpcServer(std::unique_ptr<IpcSession> session,
                                   MarketDataGateway*          md,
                                   TradingGateway*             td,
                                   asio::any_io_executor       ex,
                                   Options                     options)
    : GatewayIpcServer(std::move(session), md, td, ex, ex, std::move(options)) {}

GatewayIpcServer::GatewayIpcServer(std::unique_ptr<IpcSession> session,
                                   MarketDataGateway*          md,
                                   TradingGateway*             td,
                                   asio::any_io_executor       ex,
                                   asio::any_io_executor       gateway_ex,
                                   Options                     options)
    : session_(std::move(session)),
      md_(md),
      td_(td),
      ex_(std::move(ex)),
      gateway_ex_(std::move(gateway_ex)),
      md_sink_(std::make_shared<Channel<MarketDataEvent>>(ex_, 2048)),
      td_sink_(std::make_shared<Channel<TradeEvent>>(ex_, 2048)),
      stream_epoch_(make_stream_epoch("gw")),
      md_stream_(StreamKind::Md, stream_epoch_, options.md_replay),
      td_stream_(StreamKind::Td, stream_epoch_, options.td_replay),
      md_stop_timer_(ex_),
      td_stop_timer_(ex_) {}

GatewayIpcServer::~GatewayIpcServer() {
    // 只关闭会话，不 join 后台协程（与 IpcSession 契约一致）。
    close();
}

void GatewayIpcServer::attach_buses(EventBus<MarketDataEvent>* md_bus,
                                    EventBus<TradeEvent>*      td_bus) {
    md_bus_ = md_bus;
    td_bus_ = td_bus;
    if (md_bus_ != nullptr) {
        md_bus_->add_sink(md_sink_);
    }
    if (td_bus_ != nullptr) {
        td_bus_->add_sink(td_sink_);
    }
}

void GatewayIpcServer::start() {
    if (started_ || closed_) {
        return;
    }
    started_ = true;

    // 每个后台协程持有一份 self，确保它们在运行期间本对象不被销毁。
    auto self = shared_from_this();

    if (md_ != nullptr) {
        co_spawn_logged(
            ex_, [self]() -> asio::awaitable<void> { co_await self->md_pump(); }, "Gw.md_pump");
    }
    if (td_ != nullptr) {
        co_spawn_logged(
            ex_, [self]() -> asio::awaitable<void> { co_await self->td_pump(); }, "Gw.td_pump");
    }
    co_spawn_logged(
        ex_, [self]() -> asio::awaitable<void> { co_await self->control_loop(); }, "Gw.control");
}

void GatewayIpcServer::close() noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    // 先从扇出总线摘除，再关闭自己的 sink；避免 bus 继续向已析构连接投递。
    if (md_bus_ != nullptr) {
        md_bus_->remove_sink(md_sink_.get());
        md_bus_ = nullptr;
    }
    if (td_bus_ != nullptr) {
        td_bus_->remove_sink(td_sink_.get());
        td_bus_ = nullptr;
    }
    md_sink_->close();
    td_sink_->close();

    // 唤醒阻塞在网关 output 上的 pump（wait_stop 分支）。
    md_stop_timer_.cancel();
    td_stop_timer_.cancel();
    if (session_) {
        session_->close();
    }
    // 释放本连接在 md 网关上的订阅（引用计数退订）。按值捕获订阅快照，
    // 避免 release 协程运行期间本对象被销毁导致的悬垂访问；md_ 由上层长期持有。
    if (md_ != nullptr && (!subscribed_symbols_.empty() || !subscribed_klines_.empty())) {
        auto* md     = md_;
        auto  syms   = std::move(subscribed_symbols_);
        auto  klines = std::move(subscribed_klines_);
        SPDLOG_TRACE("[Gw] close: releasing {} symbols, {} klines", syms.size(), klines.size());
        subscribed_symbols_.clear();
        subscribed_klines_.clear();
        symbol_use_.clear();
        subscribed_kline_keys_.clear();
        co_spawn_logged(
            gateway_ex_,
            [md, syms = std::move(syms), klines = std::move(klines)]() -> asio::awaitable<void> {
                try {
                    co_await md->release(syms, klines);
                } catch (const std::exception& e) {
                    SPDLOG_WARN("[Gw] release subscriptions failed: {}", e.what());
                }
            },
            "Gw.release");
    }
}

} // namespace ipc
} // namespace tqsdk
