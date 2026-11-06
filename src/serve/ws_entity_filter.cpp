#include "tqsdk/serve/ws_entity_filter.h"

#include "tqsdk/instrument/instrument_catalog.h"
#include "tqsdk/tqapi.h"

namespace tqsdk::serve {

nlohmann::json WsEntityFilter::snapshot(const TqApi&             api,
                                        const WsEntitySelection& selection,
                                        const InstrumentCatalog* catalog) const {
    nlohmann::json data = nlohmann::json::object();

    for (const auto& sym : selection.symbols) {
        if (const Quote* quote = api.data().quotes.find(sym)) {
            data["quotes"][sym] = quote_to_json(*quote, catalog);
        }
    }
    for (const auto& [key, width] : selection.klines) {
        (void)width;
        if (const KlineStream* stream = api.data().klines.find(key.first, key.second)) {
            nlohmann::json one = kline_ws_json(*stream, true);
            for (auto& [sym, durations] : one["klines"].items()) {
                for (auto& [duration, node] : durations.items()) {
                    data["klines"][sym][duration] = std::move(node);
                }
            }
        }
    }

    // 交易类快照只发给显式请求的 topic，同时记录该客户端已看到的 JSON。
    if (selection.topics.contains("account")) {
        data["account"]    = account_to_json(api.data().account);
        sent_account_json_ = data["account"].dump();
    }
    if (selection.topics.contains("orders")) {
        data["orders"] = nlohmann::json::object();
        for (const auto& [order_id, order] : api.data().orders) {
            data["orders"][order_id]   = order_to_json(order);
            sent_order_json_[order_id] = data["orders"][order_id].dump();
        }
    }
    if (selection.topics.contains("trades")) {
        data["trades"] = nlohmann::json::object();
        for (const auto& [trade_id, trade] : api.data().trades) {
            data["trades"][trade_id]   = trade_to_json(trade);
            sent_trade_json_[trade_id] = data["trades"][trade_id].dump();
        }
    }
    if (selection.topics.contains("positions")) {
        data["positions"] = nlohmann::json::object();
        for (const auto& [symbol, position] : api.data().positions) {
            data["positions"][symbol]   = position_to_json(position);
            sent_position_json_[symbol] = data["positions"][symbol].dump();
        }
    }
    return data;
}

nlohmann::json WsEntityFilter::filter(const WsEntitySelection& selection,
                                      const nlohmann::json&    diff) const {
    nlohmann::json out = nlohmann::json::object();

    if (auto it = diff.find("quotes"); it != diff.end()) {
        for (const auto& symbol : selection.symbols) {
            if (auto quote = it->find(symbol); quote != it->end()) {
                out["quotes"][symbol] = *quote;
            }
        }
    }
    if (auto it = diff.find("klines"); it != diff.end()) {
        for (const auto& [key, width] : selection.klines) {
            (void)width;
            auto symbol_it = it->find(key.first);
            if (symbol_it == it->end()) {
                continue;
            }
            auto duration_it = symbol_it->find(std::to_string(key.second));
            if (duration_it != symbol_it->end()) {
                out["klines"][key.first][std::to_string(key.second)] = *duration_it;
            }
        }
    }

    if (selection.topics.contains("account")) {
        if (auto it = diff.find("account"); it != diff.end()) {
            auto text = it->dump();
            if (text != sent_account_json_) {
                sent_account_json_ = std::move(text);
                out["account"]     = *it;
            }
        }
    }
    if (selection.topics.contains("orders")) {
        if (auto it = diff.find("orders"); it != diff.end()) {
            for (const auto& [order_id, order] : it->items()) {
                auto text              = order.dump();
                auto [cache, inserted] = sent_order_json_.try_emplace(order_id, text);
                if (!inserted && cache->second == text) {
                    continue;
                }
                cache->second           = std::move(text);
                out["orders"][order_id] = order;
            }
        }
    }
    if (selection.topics.contains("trades")) {
        if (auto it = diff.find("trades"); it != diff.end()) {
            for (const auto& [trade_id, trade] : it->items()) {
                auto text              = trade.dump();
                auto [cache, inserted] = sent_trade_json_.try_emplace(trade_id, text);
                if (!inserted && cache->second == text) {
                    continue;
                }
                cache->second           = std::move(text);
                out["trades"][trade_id] = trade;
            }
        }
    }
    if (selection.topics.contains("positions")) {
        if (auto it = diff.find("positions"); it != diff.end()) {
            for (const auto& [symbol, position] : it->items()) {
                auto text              = position.dump();
                auto [cache, inserted] = sent_position_json_.try_emplace(symbol, text);
                if (!inserted && cache->second == text) {
                    continue;
                }
                cache->second            = std::move(text);
                out["positions"][symbol] = position;
            }
        }
    }
    return out;
}

void WsEntityFilter::reset() {
    sent_order_json_.clear();
    sent_trade_json_.clear();
    sent_position_json_.clear();
    sent_account_json_.clear();
}

} // namespace tqsdk::serve
