#include "tqsdk/pch.h"

#include "http_route_handlers.h"
#include "http_route_helpers.h"

#include "tqsdk/constants.h"
#include "tqsdk/data/tick_store.h"
#include "tqsdk/domain/sentinel.h"
#include "tqsdk/serve/json_convert.h"

namespace tqsdk {
namespace serve {
using namespace detail;

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

asio::awaitable<json> HttpServer::RouteHandlers::kline(std::string  symbol,
                                                       std::int64_t duration_ns) {
    KlineStream* stream = api_->get_kline_serial(symbol, duration_ns);
    co_return kline_to_json(*stream);
}

} // namespace serve
} // namespace tqsdk
