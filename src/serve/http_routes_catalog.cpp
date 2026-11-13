#include "tqsdk/pch.h"

#include "http_route_handlers.h"
#include "http_route_helpers.h"

#include "tqsdk/option/option_analytics.h"
#include "tqsdk/serve/json_convert.h"
#include "tqsdk/util/json_value.h"

namespace tqsdk {
namespace serve {
using namespace detail;

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
    for (const auto& item : items) {
        out.push_back(InstrumentCatalog::to_json(item));
    }
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
        if (contract.strike_price <= 0) {
            continue;
        }
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

} // namespace serve
} // namespace tqsdk
