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
        {http::verb::get, "/accounts", make_entry([this]() -> asio::awaitable<json> {
             if (accounts_ == nullptr) {
                 co_return json{
                     {"default", "default"},
                     {"accounts",
                      json::array({json{{"account_id", "default"}, {"ready", api_->ready()}}})}};
             }
             json out = json::array();
             for (const auto& id : accounts_->account_ids()) {
                 auto api = accounts_->find_account(id);
                 if (!api)
                     continue;
                 out.push_back(json{{"account_id", id},
                                    {"ready", api->ready()},
                                    {"account", account_to_json(api->get_account())}});
             }
             co_return json{{"default", accounts_->default_account_id()},
                            {"accounts", std::move(out)}};
         })},
        {http::verb::get, "/holiday/status", make_entry([]() -> asio::awaitable<json> {
             co_return HolidayCalendar::global().status_json();
         })},
        {http::verb::get,
         "/trading-status",
         make_entry([this](Query<AccountQuery> q) -> asio::awaitable<json> {
             TqApi* selected = account_api(q.value.account);
             co_return TradingStatus::from_phase(selected->session_phase()).to_json();
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
        {http::verb::get,
         "/option/analytics/{underlying}",
         make_entry([this](std::string                 underlying,
                           Query<OptionAnalyticsQuery> query) -> asio::awaitable<json> {
             if (catalog_ == nullptr) {
                 throw HttpError{http::status::service_unavailable, catalog_not_ready()};
             }
             const auto contracts = catalog_->option_contracts(underlying, 5000);
             if (contracts.empty()) {
                 throw HttpError{http::status::not_found,
                                 json{{"error", "option chain not found"}}};
             }

             auto   spot_opt = json_value::detail::parse_double(query.value.spot);
             double spot     = spot_opt.value_or(kUnknownDouble);
             if (!std::isfinite(spot)) {
                 spot = api_->get_quote(underlying).last_price;
             }
             if (!std::isfinite(spot) || spot <= 0) {
                 throw HttpError{
                     http::status::bad_request,
                     json{{"error", "spot is required or underlying quote unavailable"}}};
             }
             const double rate = json_value::detail::parse_double(query.value.rate).value_or(0.0);
             const double dividend =
                 json_value::detail::parse_double(query.value.dividend).value_or(0.0);
             const double vol    = json_value::detail::parse_double(query.value.vol).value_or(0.2);
             const int    window = static_cast<int>(std::clamp<int64_t>(
                 json_value::detail::parse_integer<int64_t>(query.value.window).value_or(5),
                 0,
                 50));
             const double now_sec =
                 std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
                     .count();
             double expire = contracts.front().expire_datetime;
             double t      = expire > now_sec ? (expire - now_sec) / (365.25 * 86400.0) : 1e-6;

             std::vector<double>                                                   strikes;
             std::unordered_map<std::string, double>                               market_prices;
             std::unordered_map<std::string, const InstrumentCatalog::Instrument*> by_strike_right;
             for (const auto& c : contracts) {
                 if (c.strike_price <= 0)
                     continue;
                 strikes.push_back(c.strike_price);
                 const auto quote = api_->get_quote(c.instrument_id);
                 if (std::isfinite(quote.last_price) && quote.last_price > 0) {
                     market_prices[c.instrument_id] = quote.last_price;
                 }
                 by_strike_right[std::to_string(
                                     static_cast<int64_t>(std::llround(c.strike_price))) +
                                 "|" + c.option_class] = &c;
             }
             std::sort(strikes.begin(), strikes.end());
             strikes.erase(std::unique(strikes.begin(), strikes.end()), strikes.end());
             auto rows = compute_option_chain(
                 spot, strikes, t, rate, dividend, vol, market_prices, underlying, window);
             json out_rows = json::array();
             for (auto& row : rows) {
                 const auto key = std::to_string(static_cast<int64_t>(std::llround(row.strike))) +
                                  "|" + (row.right == OptionRight::CALL ? "CALL" : "PUT");
                 if (auto it = by_strike_right.find(key); it != by_strike_right.end()) {
                     row.instrument_id = it->second->instrument_id;
                 }
                 out_rows.push_back(json{
                     {"instrument_id", row.instrument_id},
                     {"right", row.right == OptionRight::CALL ? "CALL" : "PUT"},
                     {"strike", row.strike},
                     {"time_to_expiry", row.time_to_expiry},
                     {"market_price",
                      row.has_market_price ? json(row.market_price) : json(nullptr)},
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
         })},

        {http::verb::get, "/kline-cache/status", make_entry([this]() -> asio::awaitable<json> {
             co_return api_->kline_cache_status();
         })},
        {http::verb::get, "/tick-cache/status", make_entry([this]() -> asio::awaitable<json> {
             if (tick_store_ == nullptr) {
                 co_return json{{"enabled", false}, {"ready", false}};
             }
             co_return tick_store_->status_json();
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
        {http::verb::get,
         "/ticks/{symbol}",
         make_entry([this](std::string symbol, Query<TickQuery> query) -> asio::awaitable<json> {
             const std::size_t limit = query_size(query.value.limit, 200, 20000);
             json              ticks = json::array();
             if (query.value.source == "cache" && tick_store_ != nullptr) {
                 for (const auto& tick : tick_store_->load_recent(symbol, limit)) {
                     ticks.push_back(tick);
                 }
                 co_return json{
                     {"instrument_id", symbol}, {"source", "cache"}, {"ticks", std::move(ticks)}};
             }
             TickSeries* series = api_->get_tick_serial(symbol, static_cast<int>(limit));
             if (series != nullptr) {
                 for (const auto& tick : series->data) {
                     ticks.push_back(tick);
                 }
             }
             co_return json{
                 {"instrument_id", symbol}, {"source", "memory"}, {"ticks", std::move(ticks)}};
         })},
        {http::verb::get,
         "/trading-snapshot",
         make_entry([this](Query<AccountQuery> q) -> asio::awaitable<json> {
             TqApi* selected = account_api(q.value.account);
             co_return trading_snapshot(*selected);
         })},
        {http::verb::get,
         "/kline/{symbol}/{duration_ns}",
         make_entry([this](std::string symbol, std::int64_t duration_ns) -> asio::awaitable<json> {
             KlineStream* s = api_->get_kline_serial(symbol, duration_ns);
             co_return kline_to_json(*s);
         })},
        {http::verb::get,
         "/account",
         make_entry([this](Query<AccountQuery> q) -> asio::awaitable<json> {
             TqApi* selected = account_api(q.value.account);
             co_return account_to_json(selected->get_account());
         })},
        {http::verb::get,
         "/orders",
         make_entry([this](Query<AccountQuery> q) -> asio::awaitable<json> {
             TqApi* selected = account_api(q.value.account);
             co_return orders_snapshot(*selected);
         })},
        {http::verb::get,
         "/trades",
         make_entry([this](Query<AccountQuery> q) -> asio::awaitable<json> {
             TqApi* selected = account_api(q.value.account);
             co_return trades_snapshot(*selected);
         })},
        {http::verb::get,
         "/positions",
         make_entry([this](Query<AccountQuery> q) -> asio::awaitable<json> {
             TqApi* selected = account_api(q.value.account);
             co_return positions_snapshot(*selected);
         })},

        // ---- 下单 / 撤单 ----
        // body 自动反序列化为 InsertOrderReq（见 trading_gateway.h 的 from_json）。
        {http::verb::post,
         "/order",
         make_entry([this](Query<AccountQuery> q, InsertOrderReq r) -> asio::awaitable<json> {
             TqApi* selected = account_api(q.value.account);
             // co_await 网关 ack；期间 io_context 继续处理行情/其它请求。
             Order*     order   = co_await selected->insert_order(r);
             const bool success = order != nullptr && !order->is_error;
             co_return json{{"success", success},
                            {"order_id", order ? order->order_id : std::string{}},
                            {"error", order ? order->last_msg : std::string{"order rejected"}}};
         })},
        {http::verb::delete_,
         "/order/{id}",
         make_entry([this](std::string oid, Query<AccountQuery> q) -> asio::awaitable<json> {
             TqApi*     selected = account_api(q.value.account);
             CommandAck ack      = co_await selected->cancel_order(CancelOrderReq{oid, ""});
             co_return json{
                 {"success", ack.success}, {"order_id", ack.order_id}, {"error", ack.error_msg}};
         })},

        // ---- 执行任务：供 SimNow/实盘验收和策略端复用 ----
        {http::verb::post,
         "/task/target",
         make_entry([this](TargetTaskRequest r) -> asio::awaitable<json> {
             if (r.symbol.empty()) {
                 throw HttpError{http::status::bad_request, json{{"error", "symbol is required"}}};
             }
             api_->get_quote(r.symbol);
             TargetPosPriceMode mode = TargetPosPriceMode::ACTIVE;
             if (r.mode == "PASSIVE") {
                 mode = TargetPosPriceMode::PASSIVE;
             } else if (r.mode == "CUSTOM") {
                 throw HttpError{http::status::bad_request,
                                 json{{"error", "CUSTOM mode requires a C++ price_provider"}}};
             } else if (r.mode != "ACTIVE") {
                 throw HttpError{http::status::bad_request, json{{"error", "unknown task mode"}}};
             }
             TargetPosOptions options;
             options.max_volume = r.max_volume;
             const auto id      = "target-" + std::to_string(++task_seq_);
             auto task = std::make_shared<TargetPosTask>(*api_, r.symbol, mode, std::move(options));
             task->set_target_volume(r.target_volume);
             target_tasks_[id] = std::move(task);
             co_return json{{"success", true},
                            {"task_id", id},
                            {"symbol", r.symbol},
                            {"target_volume", r.target_volume}};
         })},
        {http::verb::delete_,
         "/task/target/{id}",
         make_entry([this](std::string id) -> asio::awaitable<json> {
             auto it = target_tasks_.find(id);
             if (it == target_tasks_.end()) {
                 throw HttpError{http::status::not_found, json{{"error", "target task not found"}}};
             }
             it->second->cancel();
             target_tasks_.erase(it);
             co_return json{{"success", true}, {"task_id", id}};
         })},
        {http::verb::post,
         "/task/order",
         make_entry([this](OrderTaskRequest r) -> asio::awaitable<json> {
             if (r.order.instrument_id.empty() || r.order.volume == 0) {
                 throw HttpError{http::status::bad_request, json{{"error", "invalid order task"}}};
             }
             const auto id    = "order-" + std::to_string(++task_seq_);
             auto       task  = std::make_shared<InsertOrderTask>(*api_, std::move(r.order));
             order_tasks_[id] = std::move(task);
             co_return json{{"success", true}, {"task_id", id}};
         })},
        {http::verb::delete_,
         "/task/order/{id}",
         make_entry([this](std::string id) -> asio::awaitable<json> {
             auto it = order_tasks_.find(id);
             if (it == order_tasks_.end()) {
                 throw HttpError{http::status::not_found, json{{"error", "order task not found"}}};
             }
             it->second->cancel();
             order_tasks_.erase(it);
             co_return json{{"success", true}, {"task_id", id}};
         })},
        {http::verb::get, "/tasks", make_entry([this]() -> asio::awaitable<json> {
             json targets = json::array();
             for (const auto& [id, task] : target_tasks_) {
                 targets.push_back(json{{"task_id", id},
                                        {"active", task->active()},
                                        {"target_volume", task->target_volume()}});
             }
             json orders = json::array();
             for (const auto& [id, task] : order_tasks_) {
                 orders.push_back(json{{"task_id", id},
                                       {"active", task->active()},
                                       {"filled_volume", task->filled_volume()}});
             }
             co_return json{{"targets", std::move(targets)}, {"orders", std::move(orders)}};
         })},

        // ---- 风控规则：运行时可增配；用于 SimNow 验收 ----
        {http::verb::get, "/risk/status", make_entry([this]() -> asio::awaitable<json> {
             co_return json{{"rules", api_->risk_manager().size()}};
         })},
        {http::verb::delete_, "/risk/rules", make_entry([this]() -> asio::awaitable<json> {
             api_->risk_manager().clear();
             co_return json{{"success", true}, {"rules", 0}};
         })},
        {http::verb::post,
         "/risk/rules",
         make_entry([this](RiskRuleRequest r) -> asio::awaitable<json> {
             std::shared_ptr<RiskRule> rule;
             if (r.type == "OPEN_COUNT_LIMIT") {
                 rule = std::make_shared<OpenCountsLimitRule>(r.limit, r.symbols);
             } else if (r.type == "OPEN_VOLUME_LIMIT") {
                 rule = std::make_shared<OpenVolumesLimitRule>(r.limit, r.symbols);
             } else if (r.type == "ORDER_RATE_LIMIT") {
                 rule = std::make_shared<OrderRateLimitRule>(
                     r.limit, std::chrono::milliseconds(r.window_ms));
             } else {
                 throw HttpError{http::status::bad_request, json{{"error", "unknown risk rule"}}};
             }
             api_->risk_manager().add(rule);
             co_return json{
                 {"success", true}, {"type", r.type}, {"rules", api_->risk_manager().size()}};
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
