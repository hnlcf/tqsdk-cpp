#pragma once

#include "tqsdk/serve/http_server.h"

namespace tqsdk {
namespace serve {

using json = nlohmann::json;

struct HttpServer::RouteHandlers {
    explicit RouteHandlers(HttpServer& server)
        : owner(server),
          api_(server.api_),
          accounts_(server.accounts_),
          catalog_(server.catalog_),
          tick_store_(server.tick_store_),
          continuous_(server.continuous_) {}

    TqApi* account_api(std::string_view account_id) const { return owner.account_api(account_id); }

    AccountScope account_scope(std::string_view account_id) const {
        return owner.account_scope(account_id);
    }

    asio::awaitable<ServeResult<json>> healthz();
    asio::awaitable<Response>          readyz();
    asio::awaitable<ServeResult<json>> metrics();
    asio::awaitable<ServeResult<json>> accounts();
    asio::awaitable<ServeResult<json>> holiday_status();
    asio::awaitable<ServeResult<json>> trading_status(Query<AccountQuery> query);
    asio::awaitable<Response>          catalog_status();
    asio::awaitable<ServeResult<json>> catalog_instruments(Query<CatalogQuery> query);
    asio::awaitable<ServeResult<json>> catalog_instrument(std::string instrument_id);
    asio::awaitable<ServeResult<json>> option_products();
    asio::awaitable<ServeResult<json>> option_chain(std::string underlying);
    asio::awaitable<ServeResult<json>> option_analytics(std::string                 underlying,
                                                        Query<OptionAnalyticsQuery> query);
    asio::awaitable<ServeResult<json>> kline_cache_status();
    asio::awaitable<ServeResult<json>> tick_cache_status();
    asio::awaitable<ServeResult<json>> quote(std::string symbol);
    asio::awaitable<ServeResult<json>> quotes(Query<QuoteBatchQuery> query);
    asio::awaitable<ServeResult<json>> ticks(std::string symbol, Query<TickQuery> query);
    asio::awaitable<ServeResult<json>> trading_snapshot(Query<AccountQuery> query);
    asio::awaitable<ServeResult<json>>
    kline(std::string symbol, std::int64_t duration_ns, Query<KlineQuery> query);
    asio::awaitable<ServeResult<json>> account(Query<AccountQuery> query);
    asio::awaitable<ServeResult<json>> orders(Query<AccountQuery> query);
    asio::awaitable<ServeResult<json>> trades(Query<AccountQuery> query);
    asio::awaitable<ServeResult<json>> positions(Query<AccountQuery> query);
    asio::awaitable<ServeResult<json>> insert_order(Query<AccountQuery> query,
                                                    InsertOrderReq      request);
    asio::awaitable<ServeResult<json>> cancel_order(std::string         order_id,
                                                    Query<AccountQuery> query);
    asio::awaitable<ServeResult<json>> target_create(Query<AccountQuery> query,
                                                     TargetTaskRequest   request);
    asio::awaitable<ServeResult<json>> target_delete(std::string         task_id,
                                                     Query<AccountQuery> query);
    asio::awaitable<ServeResult<json>> order_task_create(Query<AccountQuery> query,
                                                         OrderTaskRequest    request);
    asio::awaitable<ServeResult<json>> order_task_delete(std::string         task_id,
                                                         Query<AccountQuery> query);
    asio::awaitable<ServeResult<json>> tasks(Query<AccountQuery> query);
    asio::awaitable<ServeResult<json>> risk_status(Query<AccountQuery> query);
    asio::awaitable<ServeResult<json>> risk_clear(Query<AccountQuery> query);
    asio::awaitable<ServeResult<json>> risk_add(Query<AccountQuery> query, RiskRuleRequest request);
    asio::awaitable<Response>          websocket_upgrade();

    HttpServer&           owner;
    TqApi*&               api_;
    MultiAccountManager*& accounts_;
    InstrumentCatalog*&   catalog_;
    TickStore*&           tick_store_;
    ContinuousTable*&     continuous_;
};

} // namespace serve
} // namespace tqsdk
