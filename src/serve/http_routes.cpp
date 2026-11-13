#include "tqsdk/data/tick_store.h"
#include "tqsdk/option/option_analytics.h"
#include "tqsdk/serve/http_server.h"

#include "tqsdk/trading/holiday_calendar.h"
#include "tqsdk/trading/trading_status.h"
#include "http_route_handlers.h"

#include "tqsdk/domain/sentinel.h"
#include "tqsdk/serve/json_convert.h"
#include "tqsdk/serve/snapshot_service.h"
#include "tqsdk/util/json_value.h"

namespace tqsdk {
namespace serve {

/// 每个路由 handler 一个方法；setup_routes() 只保留路由表。
/// RouteHandlers 通过 owner 访问 HttpServer 状态；handler 参数仍是显式传参。

asio::awaitable<ServeResult<json>> HttpServer::RouteHandlers::healthz() {
    co_return json{{"ok", true},
                   {"package", tqsdk::kPackageName},
                   {"version", tqsdk::kVersion},
                   {"package_aliases", tqsdk::kPackageAliases}};
}

asio::awaitable<Response> HttpServer::RouteHandlers::readyz() {
    auto     body = api_->readiness_json();
    Response response;
    response.status =
        body.value("ready", false) ? http::status::ok : http::status::service_unavailable;
    response.body = std::move(body);
    co_return response;
}

asio::awaitable<ServeResult<json>> HttpServer::RouteHandlers::metrics() {
    co_return api_->metrics_json();
}

asio::awaitable<ServeResult<json>> HttpServer::RouteHandlers::accounts() {
    if (accounts_ == nullptr) {
        co_return json{
            {"default", "default"},
            {"accounts", json::array({json{{"account_id", "default"}, {"ready", api_->ready()}}})}};
    }
    json out = json::array();
    for (const auto& id : accounts_->account_ids()) {
        auto account = accounts_->find_account(id);
        if (!account) {
            continue;
        }
        out.push_back(json{{"account_id", id},
                           {"ready", account->ready()},
                           {"account", account_to_json(account->get_account())}});
    }
    co_return json{{"default", accounts_->default_account_id()}, {"accounts", std::move(out)}};
}

asio::awaitable<ServeResult<json>> HttpServer::RouteHandlers::holiday_status() {
    co_return HolidayCalendar::global().status_json();
}

asio::awaitable<ServeResult<json>>
HttpServer::RouteHandlers::trading_status(Query<AccountQuery> query) {
    TqApi* selected = account_api(query.value.account);
    co_return TradingStatus::from_phase(selected->session_phase()).to_json();
}

asio::awaitable<Response> HttpServer::RouteHandlers::websocket_upgrade() {
    co_return Response{
        http::status::upgrade_required, json{{"error", "websocket upgrade required"}}, {}};
}

void HttpServer::setup_routes() {
    route_handlers_     = std::make_shared<RouteHandlers>(*this);
    const auto handlers = route_handlers_;
    const auto ws_path  = ws_->options().path;
    router_.add({
        {http::verb::get, "/healthz", make_entry([handlers]() { return handlers->healthz(); })},
        {http::verb::get, "/readyz", make_entry([handlers]() { return handlers->readyz(); })},
        {http::verb::get, "/metrics", make_entry([handlers]() { return handlers->metrics(); })},
        {http::verb::get, "/accounts", make_entry([handlers]() { return handlers->accounts(); })},
        {http::verb::get, "/holiday/status", make_entry([handlers]() {
             return handlers->holiday_status();
         })},
        {http::verb::get, "/trading-status", make_entry([handlers](Query<AccountQuery> query) {
             return handlers->trading_status(std::move(query));
         })},
        {http::verb::get, "/catalog/status", make_entry([handlers]() {
             return handlers->catalog_status();
         })},
        {http::verb::get, "/catalog/instruments", make_entry([handlers](Query<CatalogQuery> query) {
             return handlers->catalog_instruments(std::move(query));
         })},
        {http::verb::get,
         "/catalog/instruments/{instrument_id}",
         make_entry([handlers](std::string instrument_id) {
             return handlers->catalog_instrument(std::move(instrument_id));
         })},
        {http::verb::get, "/catalog/options/products", make_entry([handlers]() {
             return handlers->option_products();
         })},
        {http::verb::get,
         "/catalog/option-chain/{underlying}",
         make_entry([handlers](std::string underlying) {
             return handlers->option_chain(std::move(underlying));
         })},
        {http::verb::get,
         "/option/analytics/{underlying}",
         make_entry([handlers](std::string underlying, Query<OptionAnalyticsQuery> query) {
             return handlers->option_analytics(std::move(underlying), std::move(query));
         })},
        {http::verb::get, "/kline-cache/status", make_entry([handlers]() {
             return handlers->kline_cache_status();
         })},
        {http::verb::get, "/tick-cache/status", make_entry([handlers]() {
             return handlers->tick_cache_status();
         })},
        {http::verb::get, "/quote/{symbol}", make_entry([handlers](std::string symbol) {
             return handlers->quote(std::move(symbol));
         })},
        {http::verb::get, "/quotes", make_entry([handlers](Query<QuoteBatchQuery> query) {
             return handlers->quotes(std::move(query));
         })},
        {http::verb::get,
         "/ticks/{symbol}",
         make_entry([handlers](std::string symbol, Query<TickQuery> query) {
             return handlers->ticks(std::move(symbol), std::move(query));
         })},
        {http::verb::get, "/trading-snapshot", make_entry([handlers](Query<AccountQuery> query) {
             return handlers->trading_snapshot(std::move(query));
         })},
        {http::verb::get,
         "/kline/{symbol}/{duration_ns}",
         make_entry(
             [handlers](std::string symbol, std::int64_t duration_ns, Query<KlineQuery> query) {
                 return handlers->kline(std::move(symbol), duration_ns, std::move(query));
             })},
        {http::verb::get, "/account", make_entry([handlers](Query<AccountQuery> query) {
             return handlers->account(std::move(query));
         })},
        {http::verb::get, "/orders", make_entry([handlers](Query<AccountQuery> query) {
             return handlers->orders(std::move(query));
         })},
        {http::verb::get, "/trades", make_entry([handlers](Query<AccountQuery> query) {
             return handlers->trades(std::move(query));
         })},
        {http::verb::get, "/positions", make_entry([handlers](Query<AccountQuery> query) {
             return handlers->positions(std::move(query));
         })},
        {http::verb::post,
         "/order",
         make_entry([handlers](Query<AccountQuery> query, InsertOrderReq request) {
             return handlers->insert_order(std::move(query), std::move(request));
         })},
        {http::verb::delete_,
         "/order/{id}",
         make_entry([handlers](std::string order_id, Query<AccountQuery> query) {
             return handlers->cancel_order(std::move(order_id), std::move(query));
         })},
        {http::verb::post,
         "/task/target",
         make_entry([handlers](Query<AccountQuery> query, TargetTaskRequest request) {
             return handlers->target_create(std::move(query), std::move(request));
         })},
        {http::verb::delete_,
         "/task/target/{id}",
         make_entry([handlers](std::string task_id, Query<AccountQuery> query) {
             return handlers->target_delete(std::move(task_id), std::move(query));
         })},
        {http::verb::post,
         "/task/order",
         make_entry([handlers](Query<AccountQuery> query, OrderTaskRequest request) {
             return handlers->order_task_create(std::move(query), std::move(request));
         })},
        {http::verb::delete_,
         "/task/order/{id}",
         make_entry([handlers](std::string task_id, Query<AccountQuery> query) {
             return handlers->order_task_delete(std::move(task_id), std::move(query));
         })},
        {http::verb::get, "/tasks", make_entry([handlers](Query<AccountQuery> query) {
             return handlers->tasks(std::move(query));
         })},
        {http::verb::get, "/risk/status", make_entry([handlers](Query<AccountQuery> query) {
             return handlers->risk_status(std::move(query));
         })},
        {http::verb::delete_, "/risk/rules", make_entry([handlers](Query<AccountQuery> query) {
             return handlers->risk_clear(std::move(query));
         })},
        {http::verb::post,
         "/risk/rules",
         make_entry([handlers](Query<AccountQuery> query, RiskRuleRequest request) {
             return handlers->risk_add(std::move(query), std::move(request));
         })},
        {http::verb::get, ws_path, make_entry([handlers]() {
             return handlers->websocket_upgrade();
         })},
    });
}

} // namespace serve
} // namespace tqsdk
