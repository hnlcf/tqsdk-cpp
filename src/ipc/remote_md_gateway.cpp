#include "tqsdk/ipc/remote_md_gateway.h"

#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {
namespace ipc {

RemoteMdGateway::RemoteMdGateway(IpcClient& client, asio::any_io_executor ex)
    : client_(client),
      ex_(ex),
      ch_(ex) {
    // 注册为 IpcClient 的行情侧处理器（必须在 client.start() 之前完成）。
    client_.set_md_handler([this](ReceivedFrame&& f) { on_frame(std::move(f)); });
}

asio::awaitable<void> RemoteMdGateway::start() {
    // 连接与分发由 IpcClient 负责；这里无额外动作。
    co_return;
}

asio::awaitable<void> RemoteMdGateway::stop() {
    ch_.close();
    co_return;
}

asio::awaitable<void> RemoteMdGateway::subscribe(const std::vector<std::string>& symbols) {
    SubscribeBody body;
    body.symbols = symbols;
    co_await client_.subscribe(body);
}

asio::awaitable<void>
RemoteMdGateway::subscribe_kline(const std::string& symbol, int64_t duration_ns, int view_width) {
    SubscribeBody body;
    body.klines.push_back(KlineSub{symbol, duration_ns, view_width});
    co_await client_.subscribe(body);
}

void RemoteMdGateway::push(MarketDataEvent ev) {
    co_spawn_logged(
        ex_,
        [this, e = std::move(ev)]() mutable -> asio::awaitable<void> {
            co_await ch_.async_send(std::move(e));
        },
        "RemoteMd.push");
}

void RemoteMdGateway::on_frame(ReceivedFrame&& frame) {
    switch (frame.type) {
    case FrameType::MarketDataPush: {
        try {
            auto body = decode_as<FrameType::MarketDataPush>(frame);
            MarketDataEvent ev;
            ev.kind    = (body.kind == MdPushKind::Kline) ? MarketDataEvent::Kind::Kline
                                                          : MarketDataEvent::Kind::Diff;
            ev.payload = std::move(body.data);
            push(std::move(ev));
        } catch (const std::exception& e) {
            SPDLOG_WARN("RemoteMdGateway: bad MarketDataPush: {}", e.what());
        }
        break;
    }
    case FrameType::GatewayEvent: {
        try {
            auto body = decode_as<FrameType::GatewayEvent>(frame);
            MarketDataEvent ev;
            switch (body.kind) {
            case GatewayEventKind::Disconnected:
                ev.kind = MarketDataEvent::Kind::Disconnected;
                break;
            case GatewayEventKind::Error:
                ev.kind      = MarketDataEvent::Kind::Error;
                ev.error_msg = body.text;
                break;
            case GatewayEventKind::Connected:
                // 行情侧暂不向外暴露 connected 事件；忽略。
                return;
            }
            push(std::move(ev));
        } catch (const std::exception& e) {
            SPDLOG_WARN("RemoteMdGateway: bad GatewayEvent: {}", e.what());
        }
        break;
    }
    default:
        // IpcClient 已按类型路由；这里不该收到其它类型。
        break;
    }
}

} // namespace ipc
} // namespace tqsdk
