#include "tqsdk/ipc/gateway_subscription_book.h"

#include <algorithm>

namespace tqsdk::ipc {

std::string GatewaySubscriptionBook::kline_key(std::string_view symbol, int64_t duration_ns) {
    return fmt::format("{}|{}", symbol, duration_ns);
}

void GatewaySubscriptionBook::use_symbol(const std::string& symbol, Delta& delta) {
    if (symbol_use_[symbol]++ == 0) {
        symbols_.push_back(symbol);
        delta.symbols.push_back(symbol);
    }
}

void GatewaySubscriptionBook::release_symbol_once(const std::string& symbol, Delta& delta) {
    auto it = symbol_use_.find(symbol);
    if (it == symbol_use_.end()) {
        return;
    }
    if (--it->second <= 0) {
        symbol_use_.erase(it);
        std::erase(symbols_, symbol);
        delta.symbols.push_back(symbol);
    }
}

GatewaySubscriptionBook::Delta
GatewaySubscriptionBook::subscribe(const std::vector<std::string>&  symbols,
                                   const std::vector<KlineRequest>& klines) {
    Delta                 delta;
    std::set<std::string> explicit_symbols;
    for (const auto& symbol : symbols) {
        if (!explicit_symbols_.insert(symbol).second) {
            continue;
        }
        use_symbol(symbol, delta);
    }
    for (const auto& kline : klines) {
        if (!kline_keys_.insert(kline_key(kline.symbol, kline.duration_ns)).second) {
            continue;
        }
        use_symbol(kline.symbol, delta);
        klines_.emplace_back(kline.symbol, kline.duration_ns);
        delta.klines.emplace_back(kline.symbol, kline.duration_ns);
    }
    return delta;
}

GatewaySubscriptionBook::Delta
GatewaySubscriptionBook::unsubscribe(const std::vector<std::string>&  symbols,
                                     const std::vector<KlineRequest>& klines) {
    Delta                 delta;
    std::set<std::string> explicit_symbols;
    for (const auto& symbol : symbols) {
        if (explicit_symbols_.erase(symbol) == 0) {
            continue;
        }
        release_symbol_once(symbol, delta);
    }
    for (const auto& kline : klines) {
        if (kline_keys_.erase(kline_key(kline.symbol, kline.duration_ns)) == 0) {
            continue;
        }
        std::erase(klines_, std::make_pair(kline.symbol, kline.duration_ns));
        delta.klines.emplace_back(kline.symbol, kline.duration_ns);
        release_symbol_once(kline.symbol, delta);
    }
    return delta;
}

GatewaySubscriptionBook::Delta GatewaySubscriptionBook::release_all() {
    Delta delta;
    delta.symbols = std::move(symbols_);
    delta.klines  = std::move(klines_);
    symbols_.clear();
    klines_.clear();
    symbol_use_.clear();
    explicit_symbols_.clear();
    kline_keys_.clear();
    return delta;
}

bool GatewaySubscriptionBook::has_symbol(std::string_view symbol) const {
    return symbol_use_.contains(std::string{symbol});
}

bool GatewaySubscriptionBook::has_kline(std::string_view symbol, int64_t duration_ns) const {
    return kline_keys_.contains(kline_key(symbol, duration_ns));
}

} // namespace tqsdk::ipc
