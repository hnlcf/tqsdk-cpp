#include "tqsdk/ipc/remote_md_gateway.h"

#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {
namespace ipc {

RemoteMdGateway::RemoteMdGateway(IpcClient& client, asio::any_io_executor ex)
    : client_(client),
      ex_(ex),
      ch_(ex) {
    install_handlers();
}

RemoteMdGateway::RemoteMdGateway(std::shared_ptr<IpcClient> client, asio::any_io_executor ex)
    : client_owner_(std::move(client)),
      client_(*client_owner_),
      ex_(ex),
      ch_(ex) {
    install_handlers();
}

RemoteMdGateway::~RemoteMdGateway() {
    // 清空自己在 client 上的处理器，避免 std::function 持有悬空 this。
    // （约定：一条连接仅挂一个 md 网关，故置空不会误伤其它实例。）
    client_.set_md_handler(nullptr);
    client_.set_md_gap_handler(nullptr);
    client_.set_reconnect_handler(nullptr);
}

void RemoteMdGateway::install_handlers() {
    // 必须在 client.start() 之前完成。
    client_.set_md_handler([this](ReceivedFrame&& f) { on_frame(std::move(f)); });
    client_.set_md_gap_handler([this](const StreamGap& gap) {
        SPDLOG_WARN("[RemoteMd] seq gap on {}: epoch={} expected={} received={} missing={}; resync",
                    magic_enum::enum_name(gap.kind),
                    gap.epoch,
                    gap.expected,
                    gap.received,
                    gap.missing);
        co_spawn_logged(
            ex_, [this]() -> asio::awaitable<void> { co_await resync(); }, "RemoteMd.resync");
    });
    client_.set_reconnect_handler([this]() {
        SPDLOG_INFO("[RemoteMd] reconnect: replay subscriptions");
        co_spawn_logged(
            ex_, [this]() -> asio::awaitable<void> { co_await resync(); }, "RemoteMd.resync");
    });
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
    for (const auto& s : symbols) {
        symbols_.insert(s);
    }
}

asio::awaitable<void>
RemoteMdGateway::subscribe_kline(const std::string& symbol, int64_t duration_ns, int view_width) {
    SubscribeBody body;
    body.klines.push_back(KlineSub{symbol, duration_ns, view_width});
    co_await client_.subscribe(body);
    klines_[std::make_pair(symbol, duration_ns)] = view_width;
}

asio::awaitable<void> RemoteMdGateway::resync() {
    if (symbols_.empty() && klines_.empty()) {
        co_return;
    }
    SubscribeBody body;
    body.symbols.assign(symbols_.begin(), symbols_.end());
    body.klines.reserve(klines_.size());
    for (const auto& [key, width] : klines_) {
        // 首连时 TqApi 可能只请求最新 2 根（本地 SQLite 有历史）；断线重连
        // 无法确定停机期间缺了多少根，保守地用至少 200 根重放订阅。
        const int replay_width = std::max(width, 200);
        body.klines.push_back(KlineSub{key.first, key.second, replay_width});
    }
    SPDLOG_INFO(
        "[RemoteMd] resync: {} symbols, {} klines", body.symbols.size(), body.klines.size());
    co_await client_.subscribe(body);
}

asio::awaitable<void>
RemoteMdGateway::release(const std::vector<std::string>&                     symbols,
                         const std::vector<std::pair<std::string, int64_t>>& klines) {
    SubscribeBody body;
    body.release = true;
    body.symbols = symbols;
    body.klines.reserve(klines.size());
    for (const auto& [symbol, duration_ns] : klines) {
        body.klines.push_back(KlineSub{symbol, duration_ns, 0});
    }
    if (!body.symbols.empty() || !body.klines.empty()) {
        co_await client_.subscribe(body);
    }
    for (const auto& s : symbols) {
        symbols_.erase(s);
    }
    for (const auto& [symbol, duration_ns] : klines) {
        klines_.erase(std::make_pair(symbol, duration_ns));
    }
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
            auto            body = decode_as<FrameType::MarketDataPush>(frame);
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
            auto            body = decode_as<FrameType::GatewayEvent>(frame);
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

nlohmann::json RemoteMdGateway::metrics() const {
    return nlohmann::json{
        {"kind", "remote_md"},
        {"ipc", client_.metrics_json()},
    };
}

} // namespace ipc
} // namespace tqsdk
