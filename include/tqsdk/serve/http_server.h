#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/constants.h"

#include "tqsdk/instrument/instrument_catalog.h"
#include "tqsdk/multiaccount/multi_account_manager.h"
#include "tqsdk/serve/router.h"
#include "tqsdk/serve/ws_stream.h"
#include "tqsdk/tqapi.h"
#include "tqsdk/trading/order_tasks.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

namespace tqsdk {
class TickStore;
namespace serve {

namespace beast = boost::beast;
namespace http  = boost::beast::http;
namespace net   = boost::asio;
using tcp       = boost::asio::ip::tcp;

/// /catalog/instruments 的查询参数（全部用字符串接收再解析，兼容 ?limit=100）。
struct CatalogQuery {
    std::string classes;
    std::string exchange;
    std::string product;
    std::string q;
    std::string underlying;
    std::string include_expired;
    std::string limit;
    std::string offset;
};
inline void from_json(const json& j, CatalogQuery& q) {
    q.classes         = j.value("classes", std::string{});
    q.exchange        = j.value("exchange", std::string{});
    q.product         = j.value("product", std::string{});
    q.q               = j.value("q", std::string{});
    q.underlying      = j.value("underlying", std::string{});
    q.include_expired = j.value("include_expired", std::string{});
    q.limit           = j.value("limit", std::string{});
    q.offset          = j.value("offset", std::string{});
}

/// 执行任务 REST 配置。
struct TargetTaskRequest {
    std::string   symbol;
    std::int64_t  target_volume = 0;
    std::string   mode          = "ACTIVE";
    std::uint64_t max_volume    = 0;
};
inline void from_json(const json& j, TargetTaskRequest& r) {
    r.symbol        = j.value("symbol", std::string{});
    r.target_volume = j.value("target_volume", std::int64_t{0});
    r.mode          = j.value("mode", std::string{"ACTIVE"});
    r.max_volume    = j.value("max_volume", std::uint64_t{0});
}

struct OrderTaskRequest {
    InsertOrderReq order;
};
inline void from_json(const json& j, OrderTaskRequest& r) {
    r.order = j.value("order", InsertOrderReq{});
}

struct RiskRuleRequest {
    std::string              type;
    std::uint64_t            limit = 0;
    std::vector<std::string> symbols;
    std::uint64_t            window_ms = 1000;
};
inline void from_json(const json& j, RiskRuleRequest& r) {
    r.type      = j.value("type", std::string{});
    r.limit     = j.value("limit", std::uint64_t{0});
    r.symbols   = j.value("symbols", std::vector<std::string>{});
    r.window_ms = j.value("window_ms", std::uint64_t{1000});
}

/// 账户选择查询参数：`?account=default`。
struct AccountQuery {
    std::string account;
};
inline void from_json(const json& j, AccountQuery& q) {
    q.account = j.value("account", std::string{});
}

/// `/ticks/{symbol}` 查询参数；`source=cache` 从 TickStore 读取历史。
struct TickQuery {
    std::string limit;
    std::string source;
};
inline void from_json(const json& j, TickQuery& q) {
    q.limit  = j.value("limit", std::string{});
    q.source = j.value("source", std::string{});
}

/// `/option/analytics/{underlying}` 的估值查询参数。
struct OptionAnalyticsQuery {
    std::string spot;
    std::string rate;
    std::string dividend;
    std::string vol;
    std::string window;
};
inline void from_json(const json& j, OptionAnalyticsQuery& q) {
    q.spot     = j.value("spot", std::string{});
    q.rate     = j.value("rate", std::string{});
    q.dividend = j.value("dividend", std::string{});
    q.vol      = j.value("vol", std::string{});
    q.window   = j.value("window", std::string{});
}

/// `/quotes?symbols=A,B,C` 的批量行情查询参数。
struct QuoteBatchQuery {
    std::string symbols;
};
inline void from_json(const json& j, QuoteBatchQuery& q) {
    q.symbols = j.value("symbols", std::string{});
}

/// HttpServer —— 对外暴露的只读 + 下单 REST 接口（node 进程 serve 层）。
///
/// 这是"两进程架构"里 node 进程对外的服务边界：策略/前端通过 HTTP 拉取
/// node 内存截面（core 层 DataManager），或提交下单/撤单。serve 层直接读
/// core（同一进程内、同一 io_context 线程），**不引入额外 IPC**。
///
/// 路由（全部返回 JSON，UTF-8）：
///   GET    /healthz                       -> {"ok":true}
///   GET    /quote/{symbol}                -> Quote 快照（不存在则空占位并触发订阅）
///   GET    /kline/{symbol}/{duration_ns}  -> {"symbol","duration_ns","bars":[...]}
///   GET    /kline-cache/status             -> SQLite K 线缓存状态
///   GET    /account                       -> Account 快照
///   GET    /orders                        -> {"orders":{oid: Order,...}}
///   GET    /trades                        -> {"trades":{tid: Trade,...}}
///   GET    /positions                     -> {"positions":{symbol: Position,...}}
///   POST   /order   (body: InsertOrderReq JSON)  -> {"success","order_id","error"}
///   DELETE /order/{id}                    -> {"success","order_id","error"}
///   GET    /stream (WebSocket upgrade)    -> 实时 diff 推送（见 WebSocketHub）
///
/// 关键约定与易忽视细节：
///   - **双 executor**：I/O（accept/socket）在 io_ex 上，可为每连接分配 strand；
///     路由 handler 在 data_ex（node 中为 core strand）上执行。`session()` 在进入
///     handler 前 dispatch 到 data_ex，写回前切回连接 executor。
///     处理函数在构造响应 JSON 时对 data() 做**同步**读取并立即拷贝成 JSON，
///     不在 co_await 之间持有 data() 内部引用（下单会 co_await，故读与写分离）。
///   - TqApi::insert_order 会 co_await 网关 ack；这期间 io_context 继续处理
///     其它请求与行情，安全。
///   - 订阅语义：GET /quote 首次访问会触发一次订阅（get_quote 内部保证）。
///     要"curl 即有数据"，建议 node 启动时按配置预订阅（见 node 配置 symbols）。
///   - 请求体上限 kMaxBody（默认 1MB），防止恶意大包。
///   - 所有错误以 4xx/5xx + {"error": "..."} 返回，不抛到顶层。
class HttpServer {
public:
    /// api 与 executor 均由调用方持有，生命周期需覆盖本对象。
    /// ws_options 控制同一端口上的 WebSocket `/stream` 升级路由。
    /// 兼容构造：I/O 与数据处理使用同一 executor（单线程/测试）。
    HttpServer(TqApi*                api,
               asio::any_io_executor ex,
               WsOptions             ws_options = {},
               InstrumentCatalog*    catalog    = nullptr,
               MultiAccountManager*  accounts   = nullptr,
               TickStore*            tick_store = nullptr);

    /// 双 executor 构造：
    ///   - io_ex      ：accept / socket 读写（可跨多线程、每连接一条 strand）
    ///   - data_ex    ：路由 handler / DataManager 访问（node 中传 core strand）
    /// 所有跨 executor 切换在 session() 内显式 dispatch，保证数据面串行。
    HttpServer(TqApi*                api,
               asio::any_io_executor io_ex,
               WsOptions             ws_options,
               asio::any_io_executor data_ex,
               InstrumentCatalog*    catalog    = nullptr,
               MultiAccountManager*  accounts   = nullptr,
               TickStore*            tick_store = nullptr);

    /// 绑定并监听 addr:port；开始 accept 循环（后台协程）。失败抛 system_error。
    /// 需在 io_context.run() 之前调用。
    void listen(const std::string& address, uint16_t port);

    /// 停止监听并关闭 acceptor（幂等）。
    void stop() noexcept;

    /// 实际监听的端口（port=0 时由系统分配，可通过此值读取）。listen 后有效。
    uint16_t port() const noexcept { return port_; }

private:
    asio::awaitable<void> accept_loop();
    asio::awaitable<void> session(tcp::socket sock);

    /// 处理单个已解析请求，返回响应。内部委托给 router_ 做路由分发。
    asio::awaitable<http::response<http::string_body>> handle(http::request<http::string_body> req);

    /// 便捷：构造 JSON 响应。
    http::response<http::string_body>
    make_json_response(http::status code, const nlohmann::json& body, unsigned version);

    /// 注册全部路由（构造时调用一次）。
    void   setup_routes();
    TqApi* account_api(std::string_view account_id) const;

    /// Select the WebSocket hub for `?account=...`; no query uses the default
    /// account. Returns nullptr for an unknown account.
    std::shared_ptr<WebSocketHub> ws_hub_for(std::string_view account_id) const;

    /// 路由 handler 集合；把大 lambda 从 setup_routes 中移出。
    struct RouteHandlers;
    std::shared_ptr<RouteHandlers> route_handlers_;

    TqApi*               api_;
    MultiAccountManager* accounts_   = nullptr;
    TickStore*           tick_store_ = nullptr;
    InstrumentCatalog*   catalog_    = nullptr;

    std::uint64_t                                                     task_seq_ = 0;
    std::unordered_map<std::string, std::shared_ptr<TargetPosTask>>   target_tasks_;
    std::unordered_map<std::string, std::shared_ptr<InsertOrderTask>> order_tasks_;

    asio::any_io_executor         io_ex_;
    asio::any_io_executor         data_ex_;
    tcp::acceptor                 acceptor_;
    uint16_t                      port_{0};
    std::atomic<bool>             stopped_{false};
    Router                        router_;
    std::shared_ptr<WebSocketHub> ws_;
    std::shared_ptr<WebSocketHub> ws_default_;
    std::unordered_map<std::string, std::shared_ptr<WebSocketHub>> ws_hubs_;

    static constexpr std::size_t kMaxBody = constants::kMaxHttpBodyBytes;
};

} // namespace serve
} // namespace tqsdk
