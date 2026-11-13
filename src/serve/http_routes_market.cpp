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
        if (end == std::string::npos) {
            break;
        }
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
        for (const auto& tick : series->data) {
            ticks.push_back(tick);
        }
    }
    co_return json{{"instrument_id", symbol}, {"source", "memory"}, {"ticks", std::move(ticks)}};
}

namespace {

constexpr std::int64_t kOneDayNs = 86'400'000'000'000LL;

KlineAdjustMode parse_adjust(std::string_view value, bool& valid) {
    valid = true;
    if (value.empty() || value == "none") {
        return KlineAdjustMode::None;
    }
    if (value == "forward") {
        return KlineAdjustMode::Forward;
    }
    if (value == "back") {
        return KlineAdjustMode::Back;
    }
    valid = false;
    return KlineAdjustMode::None;
}

asio::awaitable<std::vector<KlineBar>>
load_contract_daily(TqApi* api, std::string contract, std::int64_t duration_ns) {
    auto* stream = api->get_kline_serial(contract, duration_ns, 200);
    if (stream == nullptr) {
        co_return std::vector<KlineBar>{};
    }
    for (int attempt = 0; attempt < 30 && stream->bars.empty(); ++attempt) {
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(100));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return std::vector<KlineBar>(stream->bars.begin(), stream->bars.end());
}

} // namespace

asio::awaitable<json> HttpServer::RouteHandlers::kline(std::string       symbol,
                                                       std::int64_t      duration_ns,
                                                       Query<KlineQuery> query) {
    const int    width  = static_cast<int>(std::clamp<int64_t>(
        json_value::detail::parse_integer<int64_t>(query.value.width).value_or(200), 200, 5000));
    KlineStream* stream = api_->get_kline_serial(symbol, duration_ns, width);
    if (stream == nullptr) {
        throw HttpError{http::status::service_unavailable,
                        json{{"error", "kline stream unavailable"}}};
    }
    // 首次订阅是异步的；给行情一个短暂窗口，避免首次 HTTP 请求返回空 bars。
    for (int attempt = 0; attempt < 30 && stream->bars.empty(); ++attempt) {
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(100));
        co_await timer.async_wait(asio::use_awaitable);
    }
    bool       valid = false;
    const auto mode  = parse_adjust(query.value.adjust, valid);
    if (!valid) {
        throw HttpError{http::status::bad_request,
                        json{{"error", "adjust must be none, forward or back"}}};
    }
    if (mode == KlineAdjustMode::None || duration_ns != kOneDayNs || continuous_ == nullptr ||
        !continuous_->ready()) {
        co_return kline_to_json(*stream);
    }
    if (!continuous_->is_main_continuous(symbol)) {
        co_return kline_to_json(*stream);
    }

    const std::string product = continuous_->product_of(symbol);
    std::string       first_date;
    std::string       last_date;
    if (!stream->bars.empty()) {
        first_date = ContinuousTable::date_key(stream->bars.front().datetime);
        last_date  = ContinuousTable::date_key(stream->bars.back().datetime);
    }
    const auto rolls = continuous_->rolls_for(product, first_date, last_date);
    if (rolls.empty()) {
        auto out            = kline_to_json(*stream);
        out["adjust"]       = (mode == KlineAdjustMode::Forward) ? "forward" : "back";
        out["adjust_error"] = "no roll found for visible bars";
        co_return out;
    }

    std::map<std::string, std::vector<KlineBar>> contract_bars;
    for (const auto& roll : rolls) {
        for (const auto& contract : {roll.from, roll.to}) {
            if (contract_bars.contains(contract)) {
                continue;
            }
            contract_bars[contract] = co_await load_contract_daily(api_, contract, duration_ns);
        }
    }
    auto adjusted = ContinuousTable::adjust_daily(
        std::vector<KlineBar>(stream->bars.begin(), stream->bars.end()),
        rolls,
        contract_bars,
        mode);
    KlineStream copy = *stream;
    copy.bars.assign(adjusted.bars.begin(), adjusted.bars.end());
    auto out               = kline_to_json(copy);
    out["adjust"]          = (mode == KlineAdjustMode::Forward) ? "forward" : "back";
    out["adjusted"]        = adjusted.adjusted;
    out["adjust_warnings"] = adjusted.warnings;
    co_return out;
}

} // namespace serve
} // namespace tqsdk
