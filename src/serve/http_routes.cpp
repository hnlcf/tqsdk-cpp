#include "tqsdk/data/tick_store.h"
#include "tqsdk/option/option_analytics.h"
#include "tqsdk/serve/http_server.h"
#include "tqsdk/trading/holiday_calendar.h"
#include "tqsdk/trading/trading_status.h"

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

/// 每个路由 handler 一个方法；setup_routes() 只保留路由表。
/// RouteHandlers 通过 owner 访问 HttpServer 状态；handler 参数仍是显式传参。
struct HttpServer::RouteHandlers {
    explicit RouteHandlers(HttpServer& server)
        : owner(server),
          api_(server.api_),
          accounts_(server.accounts_),
          catalog_(server.catalog_),
          tick_store_(server.tick_store_),
          target_tasks_(server.target_tasks_),
          order_tasks_(server.order_tasks_),
          task_seq_(server.task_seq_) {}

    TqApi* account_api(std::string_view account_id) const { return owner.account_api(account_id); }

    asio::awaitable<json>     healthz();
    asio::awaitable<Response> readyz();
    asio::awaitable<json>     metrics();
    asio::awaitable<json>     accounts();
    asio::awaitable<json>     holiday_status();
    asio::awaitable<json>     trading_status(Query<AccountQuery> query);
    asio::awaitable<Response> catalog_status();
    asio::awaitable<json>     catalog_instruments(Query<CatalogQuery> query);
    asio::awaitable<json>     catalog_instrument(std::string instrument_id);
    asio::awaitable<json>     option_products();
    asio::awaitable<json>     option_chain(std::string underlying);
    asio::awaitable<json>     option_analytics(std::string                 underlying,
                                               Query<OptionAnalyticsQuery> query);
    asio::awaitable<json>     kline_cache_status();
    asio::awaitable<json>     tick_cache_status();
    asio::awaitable<json>     quote(std::string symbol);
    asio::awaitable<json>     quotes(Query<QuoteBatchQuery> query);
    asio::awaitable<json>     ticks(std::string symbol, Query<TickQuery> query);
    asio::awaitable<json>     trading_snapshot(Query<AccountQuery> query);
    asio::awaitable<json>     kline(std::string symbol, std::int64_t duration_ns);
    asio::awaitable<json>     account(Query<AccountQuery> query);
    asio::awaitable<json>     orders(Query<AccountQuery> query);
    asio::awaitable<json>     trades(Query<AccountQuery> query);
    asio::awaitable<json>     positions(Query<AccountQuery> query);
    asio::awaitable<json>     insert_order(Query<AccountQuery> query, InsertOrderReq request);
    asio::awaitable<json>     cancel_order(std::string order_id, Query<AccountQuery> query);
    asio::awaitable<json>     target_create(TargetTaskRequest request);
    asio::awaitable<json>     target_delete(std::string task_id);
    asio::awaitable<json>     order_task_create(OrderTaskRequest request);
    asio::awaitable<json>     order_task_delete(std::string task_id);
    asio::awaitable<json>     tasks();
    asio::awaitable<json>     risk_status();
    asio::awaitable<json>     risk_clear();
    asio::awaitable<json>     risk_add(RiskRuleRequest request);
    asio::awaitable<Response> websocket_upgrade();

    HttpServer&                                                        owner;
    TqApi*&                                                            api_;
    MultiAccountManager*&                                              accounts_;
    InstrumentCatalog*&                                                catalog_;
    TickStore*&                                                        tick_store_;
    std::unordered_map<std::string, std::shared_ptr<TargetPosTask>>&   target_tasks_;
    std::unordered_map<std::string, std::shared_ptr<InsertOrderTask>>& order_tasks_;
    std::uint64_t&                                                     task_seq_;
};

asio::awaitable<json> HttpServer::RouteHandlers::healthz() {
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

asio::awaitable<json> HttpServer::RouteHandlers::metrics() {
    co_return api_->metrics_json();
}

asio::awaitable<json> HttpServer::RouteHandlers::accounts() {
    if (accounts_ == nullptr) {
        co_return json{
            {"default", "default"},
            {"accounts", json::array({json{{"account_id", "default"}, {"ready", api_->ready()}}})}};
    }
    json out = json::array();
    for (const auto& id : accounts_->account_ids()) {
        auto account = accounts_->find_account(id);
        if (!account)
            continue;
        out.push_back(json{{"account_id", id},
                           {"ready", account->ready()},
                           {"account", account_to_json(account->get_account())}});
    }
    co_return json{{"default", accounts_->default_account_id()}, {"accounts", std::move(out)}};
}

asio::awaitable<json> HttpServer::RouteHandlers::holiday_status() {
    co_return HolidayCalendar::global().status_json();
}

asio::awaitable<json> HttpServer::RouteHandlers::trading_status(Query<AccountQuery> query) {
    TqApi* selected = account_api(query.value.account);
    co_return TradingStatus::from_phase(selected->session_phase()).to_json();
}

asio::awaitable<Response> HttpServer::RouteHandlers::catalog_status() {
    if (catalog_ == nullptr) {
        co_return Response{http::status::service_unavailable, catalog_not_ready(), {}};
    }
    co_return Response{http::status::ok, catalog_->status_json(), {}};
}

asio::awaitable<json> HttpServer::RouteHandlers::catalog_instruments(Query<CatalogQuery> query) {
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
    const auto items       = catalog_->list(filter);
    json       out         = json::array();
    for (const auto& item : items)
        out.push_back(InstrumentCatalog::to_json(item));
    co_return json{{"instruments", std::move(out)},
                   {"count", items.size()},
                   {"catalog", catalog_->status_json()}};
}

asio::awaitable<json> HttpServer::RouteHandlers::catalog_instrument(std::string instrument_id) {
    if (catalog_ == nullptr) {
        throw HttpError{http::status::service_unavailable, catalog_not_ready()};
    }
    auto item = catalog_->find(instrument_id);
    if (!item) {
        throw HttpError{http::status::not_found, json{{"error", "instrument not found"}}};
    }
    co_return InstrumentCatalog::to_json(*item);
}

asio::awaitable<json> HttpServer::RouteHandlers::option_products() {
    if (catalog_ == nullptr) {
        throw HttpError{http::status::service_unavailable, catalog_not_ready()};
    }
    const auto products = catalog_->option_products();
    json       out      = json::array();
    for (const auto& product : products) {
        out.push_back(json{{"exchange", product.exchange},
                           {"product", product.product},
                           {"product_name", product.product_name},
                           {"underlying_symbol", product.underlying_symbol},
                           {"expire_datetime", product.expire_datetime},
                           {"calls", product.calls},
                           {"puts", product.puts}});
    }
    co_return json{{"products", std::move(out)}, {"catalog", catalog_->status_json()}};
}

asio::awaitable<json> HttpServer::RouteHandlers::option_chain(std::string underlying) {
    if (catalog_ == nullptr) {
        throw HttpError{http::status::service_unavailable, catalog_not_ready()};
    }
    const auto contracts = catalog_->option_contracts(underlying, 2000);
    if (contracts.empty()) {
        throw HttpError{http::status::not_found, json{{"error", "option chain not found"}}};
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
}

asio::awaitable<json>
HttpServer::RouteHandlers::option_analytics(std::string                 underlying,
                                            Query<OptionAnalyticsQuery> query) {
    if (catalog_ == nullptr) {
        throw HttpError{http::status::service_unavailable, catalog_not_ready()};
    }
    const auto contracts = catalog_->option_contracts(underlying, 5000);
    if (contracts.empty()) {
        throw HttpError{http::status::not_found, json{{"error", "option chain not found"}}};
    }

    auto   spot_opt = json_value::detail::parse_double(query.value.spot);
    double spot     = spot_opt.value_or(kUnknownDouble);
    if (!std::isfinite(spot)) {
        spot = api_->get_quote(underlying).last_price;
    }
    if (!std::isfinite(spot) || spot <= 0) {
        throw HttpError{http::status::bad_request,
                        json{{"error", "spot is required or underlying quote unavailable"}}};
    }
    const double rate     = json_value::detail::parse_double(query.value.rate).value_or(0.0);
    const double dividend = json_value::detail::parse_double(query.value.dividend).value_or(0.0);
    const double vol      = json_value::detail::parse_double(query.value.vol).value_or(0.2);
    const int    window   = static_cast<int>(std::clamp<int64_t>(
        json_value::detail::parse_integer<int64_t>(query.value.window).value_or(5), 0, 50));
    const double now_sec =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    double expire = contracts.front().expire_datetime;
    double t      = expire > now_sec ? (expire - now_sec) / (365.25 * 86400.0) : 1e-6;

    std::vector<double>                                                   strikes;
    std::unordered_map<std::string, double>                               market_prices;
    std::unordered_map<std::string, const InstrumentCatalog::Instrument*> by_strike_right;
    for (const auto& contract : contracts) {
        if (contract.strike_price <= 0)
            continue;
        strikes.push_back(contract.strike_price);
        const auto quote = api_->get_quote(contract.instrument_id);
        if (std::isfinite(quote.last_price) && quote.last_price > 0) {
            market_prices[contract.instrument_id] = quote.last_price;
        }
        by_strike_right[std::to_string(static_cast<int64_t>(std::llround(contract.strike_price))) +
                        "|" + contract.option_class] = &contract;
    }
    std::sort(strikes.begin(), strikes.end());
    strikes.erase(std::unique(strikes.begin(), strikes.end()), strikes.end());
    auto rows = compute_option_chain(
        spot, strikes, t, rate, dividend, vol, market_prices, underlying, window);
    json out_rows = json::array();
    for (auto& row : rows) {
        const auto key = std::to_string(static_cast<int64_t>(std::llround(row.strike))) + "|" +
                         (row.right == OptionRight::CALL ? "CALL" : "PUT");
        if (auto it = by_strike_right.find(key); it != by_strike_right.end()) {
            row.instrument_id = it->second->instrument_id;
        }
        out_rows.push_back(json{
            {"instrument_id", row.instrument_id},
            {"right", row.right == OptionRight::CALL ? "CALL" : "PUT"},
            {"strike", row.strike},
            {"time_to_expiry", row.time_to_expiry},
            {"market_price", row.has_market_price ? json(row.market_price) : json(nullptr)},
            {"implied_volatility",
             row.implied_volatility ? json(*row.implied_volatility) : json(nullptr)},
            {"price", row.valuation.price},
            {"intrinsic", row.valuation.intrinsic},
            {"time_value", row.valuation.time_value},
            {"greeks",
             json{{"delta", row.valuation.greeks.delta},
                  {"gamma", row.valuation.greeks.gamma},
                  {"vega", row.valuation.greeks.vega},
                  {"theta", row.valuation.greeks.theta},
                  {"rho", row.valuation.greeks.rho}}},
        });
    }
    co_return json{{"underlying", underlying},
                   {"spot", spot},
                   {"expire_datetime", expire},
                   {"time_to_expiry", t},
                   {"rate", rate},
                   {"dividend", dividend},
                   {"vol", vol},
                   {"atm_strike", atm_strike(spot, strikes).value_or(0.0)},
                   {"rows", std::move(out_rows)}};
}

asio::awaitable<json> HttpServer::RouteHandlers::kline_cache_status() {
    co_return api_->kline_cache_status();
}

asio::awaitable<json> HttpServer::RouteHandlers::tick_cache_status() {
    if (tick_store_ == nullptr) {
        co_return json{{"enabled", false}, {"ready", false}};
    }
    co_return tick_store_->status_json();
}

asio::awaitable<json> HttpServer::RouteHandlers::quote(std::string symbol) {
    Quote& quote = api_->get_quote(symbol);
    co_return quote_to_json(quote, catalog_);
}

asio::awaitable<json> HttpServer::RouteHandlers::quotes(Query<QuoteBatchQuery> query) {
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
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    co_return out;
}

asio::awaitable<json> HttpServer::RouteHandlers::ticks(std::string symbol, Query<TickQuery> query) {
    const std::size_t limit = query_size(query.value.limit, constants::kDefaultTickCapacity, 20000);
    json              ticks = json::array();
    if (query.value.source == "cache" && tick_store_ != nullptr) {
        for (const auto& tick : tick_store_->load_recent(symbol, limit)) {
            ticks.push_back(tick);
        }
        co_return json{{"instrument_id", symbol}, {"source", "cache"}, {"ticks", std::move(ticks)}};
    }
    TickSeries* series = api_->get_tick_serial(symbol, static_cast<int>(limit));
    if (series != nullptr) {
        for (const auto& tick : series->data)
            ticks.push_back(tick);
    }
    co_return json{{"instrument_id", symbol}, {"source", "memory"}, {"ticks", std::move(ticks)}};
}

asio::awaitable<json> HttpServer::RouteHandlers::trading_snapshot(Query<AccountQuery> query) {
    TqApi* selected = account_api(query.value.account);
    co_return ::tqsdk::serve::trading_snapshot(*selected);
}

asio::awaitable<json> HttpServer::RouteHandlers::kline(std::string  symbol,
                                                       std::int64_t duration_ns) {
    KlineStream* stream = api_->get_kline_serial(symbol, duration_ns);
    co_return kline_to_json(*stream);
}

asio::awaitable<json> HttpServer::RouteHandlers::account(Query<AccountQuery> query) {
    TqApi* selected = account_api(query.value.account);
    co_return account_to_json(selected->get_account());
}

asio::awaitable<json> HttpServer::RouteHandlers::orders(Query<AccountQuery> query) {
    TqApi* selected = account_api(query.value.account);
    co_return orders_snapshot(*selected);
}

asio::awaitable<json> HttpServer::RouteHandlers::trades(Query<AccountQuery> query) {
    TqApi* selected = account_api(query.value.account);
    co_return trades_snapshot(*selected);
}

asio::awaitable<json> HttpServer::RouteHandlers::positions(Query<AccountQuery> query) {
    TqApi* selected = account_api(query.value.account);
    co_return positions_snapshot(*selected);
}

asio::awaitable<json> HttpServer::RouteHandlers::insert_order(Query<AccountQuery> query,
                                                              InsertOrderReq      request) {
    TqApi*     selected = account_api(query.value.account);
    Order*     order    = co_await selected->insert_order(request);
    const bool success  = order != nullptr && !order->is_error;
    co_return json{{"success", success},
                   {"order_id", order ? order->order_id : std::string{}},
                   {"error", order ? order->last_msg : std::string{"order rejected"}}};
}

asio::awaitable<json> HttpServer::RouteHandlers::cancel_order(std::string         order_id,
                                                              Query<AccountQuery> query) {
    TqApi*     selected = account_api(query.value.account);
    CommandAck ack      = co_await selected->cancel_order(CancelOrderReq{order_id, ""});
    co_return json{{"success", ack.success}, {"order_id", ack.order_id}, {"error", ack.error_msg}};
}

asio::awaitable<json> HttpServer::RouteHandlers::target_create(TargetTaskRequest request) {
    if (request.symbol.empty()) {
        throw HttpError{http::status::bad_request, json{{"error", "symbol is required"}}};
    }
    api_->get_quote(request.symbol);
    TargetPosPriceMode mode = TargetPosPriceMode::ACTIVE;
    if (request.mode == "PASSIVE") {
        mode = TargetPosPriceMode::PASSIVE;
    } else if (request.mode == "CUSTOM") {
        throw HttpError{http::status::bad_request,
                        json{{"error", "CUSTOM mode requires a C++ price_provider"}}};
    } else if (request.mode != "ACTIVE") {
        throw HttpError{http::status::bad_request, json{{"error", "unknown task mode"}}};
    }
    TargetPosOptions options;
    options.max_volume = request.max_volume;
    const auto id      = "target-" + std::to_string(++task_seq_);
    auto task = std::make_shared<TargetPosTask>(*api_, request.symbol, mode, std::move(options));
    task->set_target_volume(request.target_volume);
    target_tasks_[id] = std::move(task);
    co_return json{{"success", true},
                   {"task_id", id},
                   {"symbol", request.symbol},
                   {"target_volume", request.target_volume}};
}

asio::awaitable<json> HttpServer::RouteHandlers::target_delete(std::string task_id) {
    auto it = target_tasks_.find(task_id);
    if (it == target_tasks_.end()) {
        throw HttpError{http::status::not_found, json{{"error", "target task not found"}}};
    }
    it->second->cancel();
    target_tasks_.erase(it);
    co_return json{{"success", true}, {"task_id", task_id}};
}

asio::awaitable<json> HttpServer::RouteHandlers::order_task_create(OrderTaskRequest request) {
    if (request.order.instrument_id.empty() || request.order.volume == 0) {
        throw HttpError{http::status::bad_request, json{{"error", "invalid order task"}}};
    }
    const auto id    = "order-" + std::to_string(++task_seq_);
    auto       task  = std::make_shared<InsertOrderTask>(*api_, std::move(request.order));
    order_tasks_[id] = std::move(task);
    co_return json{{"success", true}, {"task_id", id}};
}

asio::awaitable<json> HttpServer::RouteHandlers::order_task_delete(std::string task_id) {
    auto it = order_tasks_.find(task_id);
    if (it == order_tasks_.end()) {
        throw HttpError{http::status::not_found, json{{"error", "order task not found"}}};
    }
    it->second->cancel();
    order_tasks_.erase(it);
    co_return json{{"success", true}, {"task_id", task_id}};
}

asio::awaitable<json> HttpServer::RouteHandlers::tasks() {
    json targets = json::array();
    for (const auto& [id, task] : target_tasks_) {
        targets.push_back(json{
            {"task_id", id}, {"active", task->active()}, {"target_volume", task->target_volume()}});
    }
    json orders = json::array();
    for (const auto& [id, task] : order_tasks_) {
        orders.push_back(json{
            {"task_id", id}, {"active", task->active()}, {"filled_volume", task->filled_volume()}});
    }
    co_return json{{"targets", std::move(targets)}, {"orders", std::move(orders)}};
}

asio::awaitable<json> HttpServer::RouteHandlers::risk_status() {
    co_return json{{"rules", api_->risk_manager().size()}};
}

asio::awaitable<json> HttpServer::RouteHandlers::risk_clear() {
    api_->risk_manager().clear();
    co_return json{{"success", true}, {"rules", 0}};
}

asio::awaitable<json> HttpServer::RouteHandlers::risk_add(RiskRuleRequest request) {
    std::shared_ptr<RiskRule> rule;
    if (request.type == "OPEN_COUNT_LIMIT") {
        rule = std::make_shared<OpenCountsLimitRule>(request.limit, request.symbols);
    } else if (request.type == "OPEN_VOLUME_LIMIT") {
        rule = std::make_shared<OpenVolumesLimitRule>(request.limit, request.symbols);
    } else if (request.type == "ORDER_RATE_LIMIT") {
        rule = std::make_shared<OrderRateLimitRule>(request.limit,
                                                    std::chrono::milliseconds(request.window_ms));
    } else {
        throw HttpError{http::status::bad_request, json{{"error", "unknown risk rule"}}};
    }
    api_->risk_manager().add(rule);
    co_return json{
        {"success", true}, {"type", request.type}, {"rules", api_->risk_manager().size()}};
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
         make_entry([handlers](std::string symbol, std::int64_t duration_ns) {
             return handlers->kline(std::move(symbol), duration_ns);
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
        {http::verb::post, "/task/target", make_entry([handlers](TargetTaskRequest request) {
             return handlers->target_create(std::move(request));
         })},
        {http::verb::delete_, "/task/target/{id}", make_entry([handlers](std::string task_id) {
             return handlers->target_delete(std::move(task_id));
         })},
        {http::verb::post, "/task/order", make_entry([handlers](OrderTaskRequest request) {
             return handlers->order_task_create(std::move(request));
         })},
        {http::verb::delete_, "/task/order/{id}", make_entry([handlers](std::string task_id) {
             return handlers->order_task_delete(std::move(task_id));
         })},
        {http::verb::get, "/tasks", make_entry([handlers]() { return handlers->tasks(); })},
        {http::verb::get, "/risk/status", make_entry([handlers]() {
             return handlers->risk_status();
         })},
        {http::verb::delete_, "/risk/rules", make_entry([handlers]() {
             return handlers->risk_clear();
         })},
        {http::verb::post, "/risk/rules", make_entry([handlers](RiskRuleRequest request) {
             return handlers->risk_add(std::move(request));
         })},
        {http::verb::get, ws_path, make_entry([handlers]() {
             return handlers->websocket_upgrade();
         })},
    });
}

} // namespace serve
} // namespace tqsdk
