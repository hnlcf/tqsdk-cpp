#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/serve/ws_protocol.h"
#include "tqsdk/tqapi.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

namespace tqsdk {
class InstrumentCatalog;
namespace serve {

namespace beast = boost::beast;
namespace http  = boost::beast::http;
namespace asio  = boost::asio;

/// WebSocket `/stream` 的可调参数。
struct WsOptions {
    /// 升级路由；与 HTTP 共用同一个监听端口。
    std::string path = "/stream";

    /// 服务端 WebSocket ping 间隔（前端应据此设置 pong 超时）。
    std::chrono::milliseconds heartbeat_interval{std::chrono::seconds(30)};

    /// 同时在线连接上限；超过后升级请求返回 503。
    std::size_t max_clients = 256;

    /// 单连接待发送队列上限；慢消费者丢最旧消息（行情可丢，避免拖垮进程）。
    std::size_t max_queue = 1024;

    /// 数据面合并窗口：wait_update 后在 core strand 上短暂等待，把同一窗口
    /// 内的行情/交易变化合并为一帧 diff。0 表示不额外延迟（测试默认）。
    std::chrono::milliseconds coalesce{0};
};

/// WebSocketHub —— node serve 层的实时推送中心。
///
/// 设计：
///   - **单一 wait_update 消费者**：hub 的后台 push_loop 是进程内唯一调用
///     TqApi::wait_update() 的协程；每次返回后把本轮 changed_fields 翻译成
///     JSON diff，再按客户端订阅过滤后广播。避免多客户端各自 wait_update 抢事件。
///   - **引用计数订阅**：多个客户端订阅同一 symbol / kline 时只向上游订阅一次；
///     最后一个客户端取消/断开时才 release，与网关引用计数语义对称。
///   - **每连接单写者**：每个连接仅 writer 协程写 socket（消息 + 心跳 ping），
///     reader 只处理入站命令并投递到该连接的待写队列，避免并发 async_write。
///   - Hub 全部状态、订阅表、队列、reader/writer 协程绑定同一 core strand；
///     多 worker 下仍串行，因此内部不加锁。
///
/// 客户端协议（文本帧，UTF-8 JSON）。消息结构定义在 `serve/ws_protocol.h`：
/// `WsClientMessage` / `WsServerMessage` 为强类型 variant，带 to_json/from_json。
///   服务端欢迎：  {"type":"welcome","protocol":1,"heartbeat_interval_ms":30000}
///   订阅：        {"op":"subscribe","symbols":["SHFE.rb2610"],
///                  "klines":[{"symbol":"SHFE.rb2610","duration_ns":60000000000,
///                             "view_width":200}],
///                  "topics":["account","orders","trades","positions"]}
///   说明：topics 缺省/空数组只订阅 quote/kline；显式列出才订阅交易实体。
///   取消订阅：    {"op":"unsubscribe","symbols":[...],"klines":[...]}
///   应用层探活：  {"op":"ping"}  ->  {"type":"pong"}
///   订阅回执：    {"type":"subscribed",...} / {"type":"unsubscribed",...}
///   首帧快照：    {"type":"snapshot","data":{"quotes":{...},"klines":{...},
///                  "account":{...},"orders":{...},"trades":{...},"positions":{...}}}
///   增量推送：    {"type":"diff","seq":N,"data":{...}}
///   错误：        {"type":"error","error":"..."}
///
/// diff.data 中：
///   quotes   完整/部分字段（只含 changed_fields 置位字段）
///   klines   {"sym":{"dur":{"reset":bool,"bars":[...]}}}；reset=false 时只含最后一根
///   account/orders/trades/positions 仅在该客户端未见过该 JSON 时出现
///   （CTP 全量查询造成的重复实体会被过滤）。
class WebSocketHub : public std::enable_shared_from_this<WebSocketHub> {
public:
    /// 兼容构造：Hub 状态与客户端 I/O 使用同一 executor（单线程/测试）。
    WebSocketHub(TqApi* api, asio::any_io_executor ex, WsOptions options = {});

    /// 双 executor 构造：
    ///   - core_ex：Hub 状态、订阅表、push_loop、broadcast 所在的 core strand；
    ///   - io_ex  ：客户端 socket I/O 的基础 executor，每客户端再派生独立 strand。
    WebSocketHub(TqApi*                api,
                 asio::any_io_executor core_ex,
                 asio::any_io_executor io_ex,
                 WsOptions             options);
    ~WebSocketHub();

    WebSocketHub(const WebSocketHub&)            = delete;
    WebSocketHub& operator=(const WebSocketHub&) = delete;

    /// 启动后台 push_loop（幂等）。须在 io_context.run() 之前调用。
    void start();

    /// 完成 HTTP -> WebSocket 升级并接管连接。失败抛异常（由 session 记录）。
    asio::awaitable<void> accept(asio::ip::tcp::socket            socket,
                                 http::request<http::string_body> req);

    /// 停止接收/关闭所有客户端，并唤醒后台 push_loop 退出（可独立于 TqApi::close 调用）。
    void stop() noexcept;

    /// 可选：为快照/增量补充合约目录中的静态字段。
    void set_catalog(const InstrumentCatalog* catalog) noexcept { catalog_ = catalog; }

    bool             full() const;
    std::size_t      client_count() const;
    const WsOptions& options() const noexcept { return options_; }

private:
    struct Client;

    asio::awaitable<void> push_loop();
    asio::awaitable<void> wait_stop();

    asio::awaitable<void> add_subscriptions(std::shared_ptr<Client> client, WsSubscribe request);

    asio::awaitable<void> remove_subscriptions(std::shared_ptr<Client> client,
                                               WsUnsubscribe           request);

    /// reader 退出时调用：从 clients_ 摘除并把该连接独占的订阅 release 给 TqApi。
    asio::awaitable<void> remove_client(uint64_t id);

    /// 当前订阅集合的完整快照（用于订阅成功后的首帧）。
    nlohmann::json snapshot_for(Client& client) const;

    /// 把 DataManager 本轮 changed_fields 翻译成全局 diff。
    nlohmann::json build_diff() const;

    /// 按客户端 symbols/klines/topics 过滤全局 diff；无相关内容返回空 object。
    nlohmann::json filter_diff(Client& client, const nlohmann::json& diff) const;

    void broadcast(const nlohmann::json& diff);

    TqApi*                   api_;
    asio::any_io_executor    ex_;    // core strand
    asio::any_io_executor    io_ex_; // 客户端 I/O 基础 executor
    asio::steady_timer       stop_timer_;
    WsOptions                options_;
    std::atomic<bool>        started_{false};
    std::atomic<bool>        stopped_{false};
    uint64_t                 next_client_id_ = 1;
    std::atomic<uint64_t>    seq_{0};
    mutable std::mutex       clients_mu_;
    const InstrumentCatalog* catalog_ = nullptr;
    // 防止 CTP 全量查询把未变化实体重复当成 diff 广播。
    mutable std::unordered_map<std::string, std::string>  last_order_json_;
    mutable std::unordered_map<std::string, std::string>  last_trade_json_;
    mutable std::unordered_map<std::string, std::string>  last_position_json_;
    mutable std::string                                   last_account_json_;
    std::unordered_map<uint64_t, std::shared_ptr<Client>> clients_;
    std::unordered_map<std::string, int>                  quote_refs_;
    struct KlineRef {
        int refs       = 0;
        int view_width = 200;
    };
    std::map<std::pair<std::string, int64_t>, KlineRef> kline_refs_;
};

} // namespace serve
} // namespace tqsdk
