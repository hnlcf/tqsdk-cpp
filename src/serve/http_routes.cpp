#include "tqsdk/serve/http_server.h"
#include "tqsdk/trading/holiday_calendar.h"

#include "tqsdk/domain/sentinel.h"
#include "tqsdk/serve/json_convert.h"
#include "tqsdk/serve/snapshot_service.h"
#include "tqsdk/util/json_value.h"

namespace tqsdk {
namespace serve {

namespace {

using json = nlohmann::json;

std::size_t query_size(std::string_view value, std::size_t fallback, std::size_t max_value) {
    if (value.empty()) {
        return fallback;
    }
    const auto parsed = json_value::parse_unsigned(value);
    return parsed ? std::min<std::size_t>(max_value, static_cast<std::size_t>(*parsed)) : fallback;
}

bool query_bool(std::string_view value, bool fallback) {
    if (value.empty()) {
        return fallback;
    }
    return json_value::parse_boolean(value).value_or(fallback);
}

json catalog_not_ready() {
    return json{{"error", "instrument catalog is not loaded"}};
}

} // namespace

void HttpServer::setup_routes() {
    // 路由以"表"的形式声明：每行 = { 方法, 路径模式, handler }。
    // 路径中 {name} 段按位置绑定到 handler 对应形参；结构化 body 自动反序列化。
    router_.add({
        // ---- 健康检查 / 只读查询 ----
        {http::verb::get, "/healthz", make_entry([]() -> asio::awaitable<json> {
             co_return json{{"ok", true}};
         })},
        {http::verb::get, "/readyz", make_entry([this]() -> asio::awaitable<Response> {
             auto     j = api_->readiness_json();
             Response r;
             r.status =
                 j.value("ready", false) ? http::status::ok : http::status::service_unavailable;
             r.body = std::move(j);
             co_return r;
         })},
        {http::verb::get, "/metrics", make_entry([this]() -> asio::awaitable<json> {
             co_return api_->metrics_json();
         })},
        {http::verb::get, "/holiday/status", make_entry([]() -> asio::awaitable<json> {
             co_return HolidayCalendar::global().status_json();
         })},
        {http::verb::get, "/catalog/status", make_entry([this]() -> asio::awaitable<Response> {
             if (catalog_ == nullptr) {
                 co_return Response{http::status::service_unavailable, catalog_not_ready(), {}};
             }
             co_return Response{http::status::ok, catalog_->status_json(), {}};
         })},
        {http::verb::get,
         "/catalog/instruments",
         make_entry([this](Query<CatalogQuery> query) -> asio::awaitable<json> {
             if (catalog_ == nullptr) {
                 throw HttpError{http::status::service_unavailable, catalog_not_ready()};
             }
             InstrumentCatalog::ListFilter filter;
             filter.classes         = query.value.classes;
             filter.exchange        = query.value.exchange;
             filter.product         = query.value.product;
             filter.query           = query.value.q;
             filter.underlying      = query.value.underlying;
             filter.include_expired = query_bool(query.value.include_expired, false);
             filter.limit           = query_size(query.value.limit, 5000, 20000);
             filter.offset          = query_size(query.value.offset, 0, 1'000'000);
             auto items             = catalog_->list(filter);
             json out               = json::array();
             for (const auto& item : items) {
                 out.push_back(InstrumentCatalog::to_json(item));
             }
             co_return json{{"instruments", std::move(out)},
                            {"count", items.size()},
                            {"catalog", catalog_->status_json()}};
         })},
        {http::verb::get,
         "/catalog/instruments/{instrument_id}",
         make_entry([this](std::string instrument_id) -> asio::awaitable<json> {
             if (catalog_ == nullptr) {
                 throw HttpError{http::status::service_unavailable, catalog_not_ready()};
             }
             auto item = catalog_->find(instrument_id);
             if (!item) {
                 throw HttpError{http::status::not_found, json{{"error", "instrument not found"}}};
             }
             co_return InstrumentCatalog::to_json(*item);
         })},
        {http::verb::get,
         "/catalog/options/products",
         make_entry([this]() -> asio::awaitable<json> {
             if (catalog_ == nullptr) {
                 throw HttpError{http::status::service_unavailable, catalog_not_ready()};
             }
             const auto products = catalog_->option_products();
             json       out      = json::array();
             for (const auto& p : products) {
                 out.push_back(json{{"exchange", p.exchange},
                                    {"product", p.product},
                                    {"product_name", p.product_name},
                                    {"underlying_symbol", p.underlying_symbol},
                                    {"expire_datetime", p.expire_datetime},
                                    {"calls", p.calls},
                                    {"puts", p.puts}});
             }
             co_return json{{"products", std::move(out)}, {"catalog", catalog_->status_json()}};
         })},
        {http::verb::get,
         "/catalog/option-chain/{underlying}",
         make_entry([this](std::string underlying) -> asio::awaitable<json> {
             if (catalog_ == nullptr) {
                 throw HttpError{http::status::service_unavailable, catalog_not_ready()};
             }
             const auto contracts = catalog_->option_contracts(underlying, 2000);
             if (contracts.empty()) {
                 throw HttpError{http::status::not_found,
                                 json{{"error", "option chain not found"}}};
             }

             json calls = json::array();
             json puts  = json::array();
             for (const auto& contract : contracts) {
                 auto item     = InstrumentCatalog::to_json(contract);
                 item["quote"] = quote_to_json(api_->get_quote(contract.instrument_id), catalog_);
                 if (contract.option_class == "CALL") {
                     calls.push_back(std::move(item));
                 } else if (contract.option_class == "PUT") {
                     puts.push_back(std::move(item));
                 }
             }
             auto underlying_item = catalog_->find(underlying);
             auto underlying_json =
                 underlying_item ? InstrumentCatalog::to_json(*underlying_item) : json::object();
             co_return json{
                 {"underlying", underlying},
                 {"underlying_contract", std::move(underlying_json)},
                 {"underlying_quote", quote_to_json(api_->get_quote(underlying), catalog_)},
                 {"expire_datetime", contracts.front().expire_datetime},
                 {"product", InstrumentCatalog::product_of(underlying)},
                 {"product_name", contracts.front().product_short_name},
                 {"calls", std::move(calls)},
                 {"puts", std::move(puts)},
             };
         })},
        {http::verb::get, "/kline-cache/status", make_entry([this]() -> asio::awaitable<json> {
             co_return api_->kline_cache_status();
         })},
        {http::verb::get,
         "/quote/{symbol}",
         make_entry([this](std::string symbol) -> asio::awaitable<json> {
             // get_quote 在不存在时创建空 Quote 并触发订阅（首次访问）。
             Quote& q = api_->get_quote(symbol);
             co_return quote_to_json(q, catalog_);
         })},
        {http::verb::get,
         "/quotes",
         make_entry([this](Query<QuoteBatchQuery> query) -> asio::awaitable<json> {
             json out          = json::object();
             out["quotes"]     = json::object();
             std::size_t count = 0;
             std::size_t begin = 0;
             while (begin <= query.value.symbols.size() && count < 500) {
                 const auto end  = query.value.symbols.find(',', begin);
                 const auto part = query.value.symbols.substr(
                     begin, end == std::string::npos ? std::string::npos : end - begin);
                 if (!part.empty()) {
                     out["quotes"][part] = quote_to_json(api_->get_quote(part), catalog_);
                     ++count;
                 }
                 if (end == std::string::npos) {
                     break;
                 }
                 begin = end + 1;
             }
             co_return out;
         })},
        {http::verb::get, "/trading-snapshot", make_entry([this]() -> asio::awaitable<json> {
             co_return trading_snapshot(*api_);
         })},
        {http::verb::get,
         "/kline/{symbol}/{duration_ns}",
         make_entry([this](std::string symbol, std::int64_t duration_ns) -> asio::awaitable<json> {
             KlineStream* s = api_->get_kline_serial(symbol, duration_ns);
             co_return kline_to_json(*s);
         })},
        {http::verb::get, "/account", make_entry([this]() -> asio::awaitable<json> {
             co_return account_to_json(api_->get_account());
         })},
        {http::verb::get, "/orders", make_entry([this]() -> asio::awaitable<json> {
             co_return orders_snapshot(*api_);
         })},
        {http::verb::get, "/trades", make_entry([this]() -> asio::awaitable<json> {
             co_return trades_snapshot(*api_);
         })},
        {http::verb::get, "/positions", make_entry([this]() -> asio::awaitable<json> {
             co_return positions_snapshot(*api_);
         })},

        // ---- 下单 / 撤单 ----
        // body 自动反序列化为 InsertOrderReq（见 trading_gateway.h 的 from_json）。
        {http::verb::post, "/order", make_entry([this](InsertOrderReq r) -> asio::awaitable<json> {
             // co_await 网关 ack；期间 io_context 继续处理行情/其它请求。
             Order*     order   = co_await api_->insert_order(r);
             const bool success = order != nullptr && !order->is_error;
             co_return json{{"success", success},
                            {"order_id", order ? order->order_id : std::string{}},
                            {"error", order ? order->last_msg : std::string{"order rejected"}}};
         })},
        {http::verb::delete_,
         "/order/{id}",
         make_entry([this](std::string oid) -> asio::awaitable<json> {
             CommandAck ack = co_await api_->cancel_order(CancelOrderReq{oid, ""});
             co_return json{
                 {"success", ack.success}, {"order_id", ack.order_id}, {"error", ack.error_msg}};
         })},

        // ---- WebSocket 入口：普通 GET 给 426，提示必须走 Upgrade ----
        {http::verb::get, ws_->options().path, make_entry([]() -> asio::awaitable<Response> {
             co_return Response{
                 http::status::upgrade_required, json{{"error", "websocket upgrade required"}}, {}};
         })},
    });
}

} // namespace serve
} // namespace tqsdk
