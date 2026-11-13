#include "tqsdk/ipc/gateway_server.h"

#include "tqsdk/util/spawn_logged.h"

#include <boost/asio/experimental/awaitable_operators.hpp>

namespace tqsdk {
namespace ipc {

using namespace boost::asio::experimental::awaitable_operators;

asio::awaitable<void> GatewayIpcServer::send_md_push(MarketDataPushBody body) {
    body.meta          = md_stream_.next_meta();
    const auto enc     = session_->encoding();
    auto       payload = encode_body(body, enc);
    md_stream_.record(body.meta.seq, FrameType::MarketDataPush, enc, payload);
    co_await session_->send(FrameType::MarketDataPush, ConstSlice{payload});
}

asio::awaitable<void> GatewayIpcServer::send_td_push(TradePushBody body) {
    body.meta          = td_stream_.next_meta();
    const auto enc     = session_->encoding();
    auto       payload = encode_body(body, enc);
    td_stream_.record(body.meta.seq, FrameType::TradePush, enc, payload);
    co_await session_->send(FrameType::TradePush, ConstSlice{payload});
}

asio::awaitable<void> GatewayIpcServer::send_td_snapshot() {
    if (td_ == nullptr) {
        co_return;
    }
    TradeSnapshotBody body;
    body.meta = td_stream_.next_meta();
    // td_->snapshot() 访问共享交易网关状态，必须在 gateway core strand 上执行。
    co_await asio::dispatch(gateway_ex_, asio::use_awaitable);
    body.data  = td_->snapshot();
    body.ready = td_->snapshot_ready();
    co_await asio::dispatch(ex_, asio::use_awaitable);
    const auto enc     = session_->encoding();
    auto       payload = encode_body(body, enc);
    td_stream_.record(body.meta.seq, FrameType::TradeSnapshot, enc, payload);
    co_await session_->send(FrameType::TradeSnapshot, ConstSlice{payload});
}

asio::awaitable<void> GatewayIpcServer::wait_stop(asio::steady_timer& t) {
    if (closed_) {
        co_return;
    }
    // 设一个极远的到期点等待 close() 取消（每个 pump 用自己的 timer，
    // 避免 expires_at 互相取消对方挂起的 wait）。
    t.expires_at(std::chrono::steady_clock::time_point::max());
    boost::system::error_code ec;
    co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    co_return;
}

asio::awaitable<void> GatewayIpcServer::md_pump() {
    Channel<MarketDataEvent>& out = (md_bus_ != nullptr) ? *md_sink_ : md_->output();
    while (!closed_) {
        // race：网关事件 vs 停止信号。若停止先到，接收方被取消并退出循环。
        std::variant<std::optional<MarketDataEvent>, std::monostate> raced;
        try {
            raced = co_await (out.async_receive() || wait_stop(md_stop_timer_));
        } catch (const boost::system::system_error& e) {
            if (e.code() == asio::error::operation_aborted) {
                co_return;
            }
            SPDLOG_WARN("[Gw.pump] receive failed: {}", e.what());
            co_return;
        }
        if (std::holds_alternative<std::monostate>(raced)) {
            co_return; // 停止信号获胜
        }
        auto ev = std::move(std::get<0>(raced));
        if (!ev.has_value()) {
            // 网关关闭了事件源：md 側结束，但不连带关闭整条连接。
            SPDLOG_DEBUG("[Gw.md_pump] md output closed");
            co_return;
        }

        // 多 node 通过 EventBus 收到同一事件后，各连接只发送自己订阅的部分。
        if (!filter_md_for_connection(*ev)) {
            continue;
        }

        try {
            switch (ev->kind) {
            case MarketDataEvent::Kind::Diff:
            case MarketDataEvent::Kind::Kline: {
                MarketDataPushBody body;
                body.kind = (ev->kind == MarketDataEvent::Kind::Kline) ? MdPushKind::Kline
                                                                       : MdPushKind::Diff;
                body.data = std::move(ev->payload);
                co_await send_md_push(std::move(body));
                break;
            }
            case MarketDataEvent::Kind::Disconnected:
                co_await session_->send_body<FrameType::GatewayEvent>(
                    GatewayEventBody{GatewayEventKind::Disconnected, std::string{}});
                break;
            case MarketDataEvent::Kind::Error:
                co_await session_->send_body<FrameType::GatewayEvent>(
                    GatewayEventBody{GatewayEventKind::Error, ev->error_msg});
                break;
            }
        } catch (const std::exception& e) {
            SPDLOG_WARN("[Gw.md_pump] send failed: {}", e.what());
            co_return;
        }
    }
    co_return;
}

// ---------------------------------------------------------------------------
// td_pump：把交易网关事件翻译成 TradePush / GatewayEvent 上推。
// ---------------------------------------------------------------------------
asio::awaitable<void> GatewayIpcServer::td_pump() {
    Channel<TradeEvent>& out = (td_bus_ != nullptr) ? *td_sink_ : td_->event_output();
    while (!closed_) {
        std::variant<std::optional<TradeEvent>, std::monostate> raced;
        try {
            raced = co_await (out.async_receive() || wait_stop(td_stop_timer_));
        } catch (const boost::system::system_error& e) {
            if (e.code() == asio::error::operation_aborted) {
                co_return;
            }
            SPDLOG_WARN("[Gw.pump] receive failed: {}", e.what());
            co_return;
        }
        if (std::holds_alternative<std::monostate>(raced)) {
            co_return; // 停止信号获胜
        }
        auto ev = std::move(std::get<0>(raced));
        if (!ev.has_value()) {
            SPDLOG_DEBUG("[Gw.td_pump] td output closed");
            co_return;
        }

        try {
            switch (ev->kind) {
            case TradeEvent::Kind::Diff: {
                TradePushBody body;
                body.kind = TradePushKind::Diff;
                body.data = std::move(ev->payload);
                co_await send_td_push(std::move(body));
                break;
            }
            case TradeEvent::Kind::Snapshot: {
                TradeSnapshotBody body;
                body.meta          = td_stream_.next_meta();
                body.data          = std::move(ev->payload);
                const auto enc     = session_->encoding();
                auto       payload = encode_body(body, enc);
                td_stream_.record(body.meta.seq, FrameType::TradeSnapshot, enc, payload);
                co_await session_->send(FrameType::TradeSnapshot, ConstSlice{payload});
                break;
            }
            case TradeEvent::Kind::Ack:
                // 命令 ack 已在 handle_command 中同步返回 CommandAck，这里不重复上推。
                break;
            case TradeEvent::Kind::Disconnected:
                co_await session_->send_body<FrameType::GatewayEvent>(
                    GatewayEventBody{GatewayEventKind::Disconnected, std::string{}});
                break;
            case TradeEvent::Kind::Error:
                co_await session_->send_body<FrameType::GatewayEvent>(
                    GatewayEventBody{GatewayEventKind::Error, ev->error_msg});
                break;
            }
        } catch (const std::exception& e) {
            SPDLOG_WARN("[Gw.td_pump] send failed: {}", e.what());
            co_return;
        }
    }
    co_return;
}

// ---------------------------------------------------------------------------
// control_loop：消费 node 下发的 Subscribe / Command。
// ---------------------------------------------------------------------------

} // namespace ipc
} // namespace tqsdk
