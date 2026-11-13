#include "tqsdk/serve/ws_stream.h"
#include "tqsdk/serve/ws_entity_filter.h"

#include "tqsdk/serve/json_convert.h"
#include "tqsdk/util/spawn_logged.h"

#include <boost/asio/experimental/awaitable_operators.hpp>

#include <variant>

namespace tqsdk {
namespace serve {

namespace {

using json = nlohmann::json;
using namespace boost::asio::experimental::awaitable_operators;

} // namespace

// ============================================================================
// 单连接
// ============================================================================
struct WebSocketHub::Client : std::enable_shared_from_this<WebSocketHub::Client> {
    using ws_t = beast::websocket::stream<beast::tcp_stream>;

    Client(uint64_t                    id,
           ws_t                        ws,
           std::weak_ptr<WebSocketHub> hub,
           asio::any_io_executor       client_ex,
           asio::any_io_executor       hub_ex,
           WsOptions                   options)
        : id_(id),
          ws_(std::move(ws)),
          hub_(std::move(hub)),
          client_ex_(std::move(client_ex)),
          hub_ex_(std::move(hub_ex)),
          wakeup_(client_ex_),
          options_(std::move(options)) {}

    void start() {
        auto self = shared_from_this();
        co_spawn_logged(
            client_ex_,
            [self]() -> asio::awaitable<void> { co_await self->writer_loop(); },
            "Ws.writer");
        co_spawn_logged(
            client_ex_,
            [self]() -> asio::awaitable<void> { co_await self->reader_loop(); },
            "Ws.reader");
    }

    /// 发送一条强类型服务端消息（自动序列化为文本帧）。
    /// 可从 hub core strand 或客户端 strand 调用；实际入队 post 到客户端 strand。
    template <typename Message>
    void send(Message msg) {
        post_text(serialize_ws_message(WsServerMessage{std::move(msg)}));
    }

    void post_text(std::string msg) {
        if (closed()) {
            return;
        }
        auto self = shared_from_this();
        asio::post(client_ex_,
                   [self, msg = std::move(msg)]() mutable { self->enqueue_local(std::move(msg)); });
    }

    /// 仅允许在 client_ex_ 上调用。队列满时丢最旧消息。
    void enqueue_local(std::string msg) {
        if (closed()) {
            return;
        }
        if (queue_.size() >= options_.max_queue) {
            queue_.pop_front();
            const auto dropped = ++dropped_;
            if (dropped == 1 || dropped % 100 == 0) {
                SPDLOG_WARN("[WsHub] client {} slow consumer, dropped={} messages", id_, dropped);
            }
        }
        queue_.push_back(std::move(msg));
        try {
            wakeup_.cancel();
        } catch (...) {
        }
    }

    /// 可从任意线程调用：置 closed 并把真正的 socket close post 到客户端 strand。
    void shutdown() noexcept {
        if (closed_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        try {
            auto self = shared_from_this();
            asio::post(client_ex_, [self]() { self->close_local(); });
        } catch (...) {
            // executor 已销毁/无效时不能再 post；socket 归属连接对象，析构时关闭。
        }
    }

    /// 仅允许在 client_ex_ 上调用；幂等。
    void close_local() noexcept {
        closed_.store(true, std::memory_order_release);
        if (socket_closed_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        try {
            wakeup_.cancel();
        } catch (...) {
        }
        // basic_stream::close() 取消所有挂起读写，read/write 协程会以 operation_aborted 退出。
        try {
            ws_.next_layer().close();
        } catch (...) {
        }
    }

    bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }

    uint64_t id() const noexcept { return id_; }

    asio::awaitable<void> reader_loop() {
        auto               self = shared_from_this();
        beast::flat_buffer buffer;
        while (!closed()) {
            boost::system::error_code ec;
            co_await ws_.async_read(buffer, asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                break;
            }
            std::string msg = beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());
            try {
                co_await handle_message(std::move(msg));
            } catch (const std::exception& e) {
                send(WsError{e.what()});
            }
        }
        close_local();
        if (auto hub = hub_.lock()) {
            // Hub 状态在 core strand；移除订阅必须切回 core。
            co_await asio::dispatch(hub->ex_, asio::use_awaitable);
            co_await hub->remove_client(id_);
        }
    }

    asio::awaitable<void> writer_loop() {
        auto                      self = shared_from_this();
        boost::system::error_code ec;
        while (!closed()) {
            if (!queue_.empty()) {
                std::string msg = std::move(queue_.front());
                queue_.pop_front();
                ec.clear();
                co_await ws_.async_write(asio::buffer(msg),
                                         asio::redirect_error(asio::use_awaitable, ec));
                if (ec) {
                    break;
                }
                continue;
            }

            // 空闲：等待新消息或心跳到期。enqueue_local() 会 cancel 该 timer 唤醒 writer。
            wakeup_.expires_after(options_.heartbeat_interval);
            ec.clear();
            co_await wakeup_.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if (closed()) {
                break;
            }
            if (ec == asio::error::operation_aborted) {
                continue; // 有新消息
            }
            if (ec) {
                break;
            }

            // 纯心跳 ping；对端 pong 由 reader 的 async_read 自动处理。
            ec.clear();
            co_await ws_.async_ping(beast::websocket::ping_data{},
                                    asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                break;
            }
        }
        close_local();
    }

    asio::awaitable<void> handle_message(std::string text) {
        auto parsed = parse_ws_client_message(text);
        if (!parsed) {
            send(WsError{parsed.error().detail});
            co_return;
        }
        WsClientMessage message = std::move(*parsed);

        // 编译期分派到各自的 handler；不会漏掉新加的 variant 成员。
        co_await std::visit([this](const auto& msg) { return handle(msg); }, message);
    }

    asio::awaitable<void> handle(const WsPing&) {
        send(WsPong{});
        co_return;
    }

    asio::awaitable<void> handle(const WsSubscribe& msg) {
        auto hub = hub_.lock();
        if (!hub) {
            send(WsError{"server shutting down"});
            co_return;
        }
        co_await asio::dispatch(hub_ex_, asio::use_awaitable);
        co_await hub->add_subscriptions(shared_from_this(), msg);
        co_await asio::dispatch(client_ex_, asio::use_awaitable);
    }

    asio::awaitable<void> handle(const WsUnsubscribe& msg) {
        auto hub = hub_.lock();
        if (!hub) {
            send(WsError{"server shutting down"});
            co_return;
        }
        co_await asio::dispatch(hub_ex_, asio::use_awaitable);
        co_await hub->remove_subscriptions(shared_from_this(), msg);
        co_await asio::dispatch(client_ex_, asio::use_awaitable);
    }

    // ---- 订阅状态（仅由 hub 在 core strand 读写）----
    std::set<std::string>                          symbols;
    std::map<std::pair<std::string, int64_t>, int> klines; // key -> view_width
    std::set<std::string>                          topics;
    bool                                           topics_initialized = false;
    // 与网络 I/O 解耦的快照/增量裁剪器；每个客户端独立缓存已发送实体。
    WsEntityFilter filter;

    uint64_t                    id_;
    ws_t                        ws_;
    std::weak_ptr<WebSocketHub> hub_;
    asio::any_io_executor       client_ex_;
    asio::any_io_executor       hub_ex_;
    asio::steady_timer          wakeup_;
    WsOptions                   options_;
    std::deque<std::string>     queue_;
    std::atomic<bool>           closed_{false};
    std::atomic<bool>           socket_closed_{false};
    uint64_t                    dropped_ = 0;
};

// ============================================================================
// Hub
// ============================================================================
WebSocketHub::WebSocketHub(TqApi* api, asio::any_io_executor ex, WsOptions options)
    : WebSocketHub(api, ex, ex, std::move(options)) {}

WebSocketHub::WebSocketHub(TqApi*                api,
                           asio::any_io_executor core_ex,
                           asio::any_io_executor io_ex,
                           WsOptions             options)
    : api_(api),
      ex_(std::move(core_ex)),
      io_ex_(std::move(io_ex)),
      stop_timer_(ex_),
      options_(std::move(options)) {
    if (options_.heartbeat_interval.count() <= 0) {
        options_.heartbeat_interval = std::chrono::seconds(30);
    }
    if (options_.max_clients == 0) {
        options_.max_clients = 1;
    }
    if (options_.max_queue == 0) {
        options_.max_queue = 1;
    }
}

WebSocketHub::~WebSocketHub() {
    stop();
}

void WebSocketHub::start() {
    if (stopped_.load(std::memory_order_acquire)) {
        return;
    }
    if (started_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    auto self = shared_from_this();
    co_spawn_logged(
        ex_, [self]() -> asio::awaitable<void> { co_await self->push_loop(); }, "WsHub.push");
}

void WebSocketHub::stop() noexcept {
    if (stopped_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    try {
        stop_timer_.cancel();
    } catch (...) {
    }
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard lock(clients_mu_);
        clients.reserve(clients_.size());
        for (auto& [id, client] : clients_) {
            (void)id;
            clients.push_back(client);
        }
    }
    for (auto& client : clients) {
        if (client) {
            client->shutdown();
        }
    }
}

bool WebSocketHub::full() const {
    std::lock_guard lock(clients_mu_);
    return clients_.size() >= options_.max_clients;
}

std::size_t WebSocketHub::client_count() const {
    std::lock_guard lock(clients_mu_);
    return clients_.size();
}

asio::awaitable<void> WebSocketHub::accept(asio::ip::tcp::socket            socket,
                                           http::request<http::string_body> req) {
    using ws_t = Client::ws_t;

    ws_t ws(std::move(socket));
    ws.read_message_max(1u << 20); // 客户端命令上限 1MB，防止恶意大包
    boost::system::error_code ec;
    co_await ws.async_accept(req, asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
        SPDLOG_WARN("[WsHub] handshake failed: {}", ec.message());
        co_return;
    }

    if (stopped_.load() || full()) {
        co_await ws.async_close(
            beast::websocket::close_reason(beast::websocket::close_code::try_again_later),
            asio::redirect_error(asio::use_awaitable, ec));
        co_return;
    }

    const uint64_t id = next_client_id_++;
    // 每客户端一条独立 strand：reader/writer/socket I/O 并行，
    // Hub 状态仍只在 core strand 访问。
    auto client_ex = asio::make_strand(io_ex_);
    auto client =
        std::make_shared<Client>(id, std::move(ws), shared_from_this(), client_ex, ex_, options_);
    std::size_t total    = 0;
    bool        stopping = false;
    {
        std::lock_guard lock(clients_mu_);
        if (stopped_.load(std::memory_order_acquire)) {
            stopping = true;
        } else {
            clients_.emplace(id, client);
            total = clients_.size();
        }
    }
    if (stopping) {
        client->shutdown();
        co_return;
    }

    WsWelcome welcome;
    welcome.protocol              = kWsProtocolVersion;
    welcome.heartbeat_interval_ms = options_.heartbeat_interval.count();
    client->send(std::move(welcome));
    client->start();
    SPDLOG_INFO("[WsHub] client {} connected (total={})", id, total);
    co_return;
}

asio::awaitable<void> WebSocketHub::wait_stop() {
    stop_timer_.expires_at(std::chrono::steady_clock::time_point::max());
    boost::system::error_code ec;
    co_await stop_timer_.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    co_return;
}

asio::awaitable<void> WebSocketHub::push_loop() {
    while (!stopped_.load()) {
        // race：数据更新 vs stop()。停止信号获胜时取消 wait_update，后者内部
        // 已处理 operation_aborted，不会留下悬垂 waiter。
        std::variant<bool, std::monostate> raced;
        try {
            raced = co_await (api_->wait_update() || wait_stop());
        } catch (const boost::system::system_error& e) {
            if (e.code() == asio::error::operation_aborted) {
                break;
            }
            SPDLOG_WARN("[WsHub] push wait failed: {}", e.what());
            break;
        }
        if (std::holds_alternative<std::monostate>(raced)) {
            break; // stop()
        }
        if (!std::get<0>(raced) || stopped_.load()) {
            break;
        }
        if (options_.coalesce.count() > 0) {
            asio::steady_timer timer(ex_);
            timer.expires_after(options_.coalesce);
            boost::system::error_code ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if (stopped_.load()) {
                break;
            }
        }
        json diff = build_diff();
        if (!diff.empty()) {
            broadcast(diff);
        }
    }
    SPDLOG_DEBUG("[WsHub] push loop exited");
    co_return;
}

asio::awaitable<void> WebSocketHub::add_subscriptions(std::shared_ptr<Client> client,
                                                      WsSubscribe             request) {
    if (!client || client->closed()) {
        co_return;
    }

    // topics：缺省只订阅 quote/kline，不默认推送账户和交易实体；
    // 显式 topics（即使空数组）整体替换。UI 会显式请求四个交易 topic。
    if (request.topics_provided) {
        client->topics = std::set<std::string>(request.topics.begin(), request.topics.end());
        client->topics_initialized = true;
    } else if (!client->topics_initialized) {
        client->topics.clear();
        client->topics_initialized = true;
    }

    std::vector<std::string>       new_symbols;
    std::vector<KlineSubscription> new_klines;

    for (const auto& sym : request.symbols) {
        if (!client->symbols.insert(sym).second) {
            continue;
        }
        auto& refs = quote_refs_[sym];
        if (refs++ == 0) {
            new_symbols.push_back(sym);
        }
    }

    for (const auto& k : request.klines) {
        const auto key = std::make_pair(k.symbol, k.duration_ns);
        if (!client->klines.emplace(key, k.view_width).second) {
            continue;
        }
        auto& ref = kline_refs_[key];
        if (ref.refs++ == 0) {
            ref.view_width = k.view_width;
            new_klines.push_back(k);
        } else if (k.view_width > ref.view_width) {
            // 已有订阅时首个宽度说了算：改本地容量不重发 subscribe_kline，
            // 否则会破坏网关侧 chart 引用计数。
            ref.view_width = k.view_width;
            if (auto* s = api_->get_kline(k.symbol, k.duration_ns)) {
                if (static_cast<int>(s->capacity) < k.view_width) {
                    s->capacity = static_cast<uint64_t>(k.view_width);
                }
            }
        }
    }

    // 先触达上游（TqApi 会创建本地占位实体），再回执 + 快照；
    // 这样客户端拿到 snapshot 时订阅实体一定存在。
    if (!new_symbols.empty()) {
        co_await api_->subscribe_quotes(std::move(new_symbols));
    }
    for (const auto& k : new_klines) {
        co_await api_->subscribe_kline(k.symbol, k.duration_ns, k.view_width);
    }

    WsSubscribed ack;
    ack.symbols = std::move(request.symbols);
    ack.klines  = std::move(request.klines);
    ack.topics.assign(client->topics.begin(), client->topics.end());
    client->send(std::move(ack));
    client->send(WsSnapshot{snapshot_for(*client)});
    co_return;
}

asio::awaitable<void> WebSocketHub::remove_subscriptions(std::shared_ptr<Client> client,
                                                         WsUnsubscribe           request) {
    if (!client) {
        co_return;
    }

    std::vector<std::string>                     release_symbols;
    std::vector<std::pair<std::string, int64_t>> release_klines;

    for (const auto& sym : request.symbols) {
        if (client->symbols.erase(sym) == 0) {
            continue;
        }
        auto it = quote_refs_.find(sym);
        if (it != quote_refs_.end() && --it->second <= 0) {
            quote_refs_.erase(it);
            release_symbols.push_back(sym);
        }
    }
    for (const auto& k : request.klines) {
        const auto key = std::make_pair(k.symbol, k.duration_ns);
        if (client->klines.erase(key) == 0) {
            continue;
        }
        auto it = kline_refs_.find(key);
        if (it != kline_refs_.end() && --it->second.refs <= 0) {
            kline_refs_.erase(it);
            release_klines.emplace_back(k.symbol, k.duration_ns);
        }
    }

    WsUnsubscribed ack;
    ack.symbols = std::move(request.symbols);
    ack.klines  = std::move(request.klines);
    client->send(std::move(ack));

    if (!release_symbols.empty() || !release_klines.empty()) {
        co_await api_->release(std::move(release_symbols), std::move(release_klines));
    }
    co_return;
}

asio::awaitable<void> WebSocketHub::remove_client(uint64_t id) {
    std::shared_ptr<Client> client;
    std::size_t             total = 0;
    {
        std::lock_guard lock(clients_mu_);
        auto            it = clients_.find(id);
        if (it == clients_.end()) {
            co_return;
        }
        client = std::move(it->second);
        clients_.erase(it);
        total = clients_.size();
    }

    std::vector<std::string>                     release_symbols;
    std::vector<std::pair<std::string, int64_t>> release_klines;

    for (const auto& sym : client->symbols) {
        auto rit = quote_refs_.find(sym);
        if (rit != quote_refs_.end() && --rit->second <= 0) {
            quote_refs_.erase(rit);
            release_symbols.push_back(sym);
        }
    }
    for (const auto& [key, width] : client->klines) {
        (void)width;
        auto rit = kline_refs_.find(key);
        if (rit != kline_refs_.end() && --rit->second.refs <= 0) {
            kline_refs_.erase(rit);
            release_klines.push_back(key);
        }
    }

    SPDLOG_INFO("[WsHub] client {} disconnected (total={})", id, total);
    if (!release_symbols.empty() || !release_klines.empty()) {
        co_await api_->release(std::move(release_symbols), std::move(release_klines));
    }
    co_return;
}

nlohmann::json WebSocketHub::snapshot_for(Client& client) const {
    WsEntitySelection selection;
    selection.symbols = client.symbols;
    selection.klines  = client.klines;
    selection.topics  = client.topics;
    return client.filter.snapshot(*api_, selection, catalog_);
}

nlohmann::json WebSocketHub::build_diff() const {
    json data = json::object();

    for (const auto& [sym, refs] : quote_refs_) {
        if (refs <= 0) {
            continue;
        }
        if (const Quote* q = api_->data().quotes.find(sym); q && q->changed_fields != 0) {
            data["quotes"][sym] = quote_diff_to_json(*q, catalog_);
        }
    }
    for (const auto& [key, ref] : kline_refs_) {
        if (ref.refs <= 0) {
            continue;
        }
        if (const KlineStream* s = api_->data().klines.find(key.first, key.second);
            s && s->changed_fields != 0) {
            json one = kline_ws_json(*s);
            for (auto& [sym, durs] : one["klines"].items()) {
                for (auto& [dur, node] : durs.items()) {
                    data["klines"][sym][dur] = std::move(node);
                }
            }
        }
    }

    const auto& dm = api_->data();
    if (dm.account.changed_fields != 0) {
        data["account"] = account_to_json(dm.account);
    }
    for (const auto& [oid, o] : dm.orders) {
        if (o.changed_fields != 0) {
            data["orders"][oid] = order_to_json(o);
        }
    }
    for (const auto& [tid, t] : dm.trades) {
        if (t.changed_fields != 0) {
            data["trades"][tid] = trade_to_json(t);
        }
    }
    for (const auto& [sym, p] : dm.positions) {
        if (p.changed_fields != 0) {
            data["positions"][sym] = position_to_json(p);
        }
    }
    return data;
}

nlohmann::json WebSocketHub::filter_diff(Client& client, const nlohmann::json& diff) const {
    WsEntitySelection selection;
    selection.symbols = client.symbols;
    selection.klines  = client.klines;
    selection.topics  = client.topics;
    return client.filter.filter(selection, diff);
}

void WebSocketHub::broadcast(const nlohmann::json& diff) {
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard lock(clients_mu_);
        clients.reserve(clients_.size());
        for (auto& [id, client] : clients_) {
            (void)id;
            if (client) {
                clients.push_back(client);
            }
        }
    }
    if (clients.empty()) {
        return;
    }
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

    for (auto& client : clients) {
        if (client->closed()) {
            continue;
        }
        json data = filter_diff(*client, diff);
        if (data.empty()) {
            continue;
        }
        WsDiff msg;
        msg.seq          = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
        msg.timestamp_ns = now_ns;
        msg.data         = std::move(data);
        client->send(std::move(msg));
    }
}

} // namespace serve
} // namespace tqsdk
