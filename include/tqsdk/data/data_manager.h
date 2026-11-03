#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/data/entity_store.h"
#include "tqsdk/domain/account.h"
#include "tqsdk/domain/kline.h"
#include "tqsdk/domain/order.h"
#include "tqsdk/domain/position.h"
#include "tqsdk/domain/quote.h"
#include "tqsdk/domain/trade.h"

namespace tqsdk {

class KlineRegistry {
public:
    static std::string make_key(std::string_view sym, int64_t dur) {
        std::string k;
        k.reserve(sym.size() + 20);
        k += sym;
        k += ':';
        k += std::to_string(dur);
        return k;
    }

    KlineStream&
    get_or_create(std::string_view sym, int64_t dur, int cap, bool* created = nullptr) {
        auto key            = make_key(sym, dur);
        auto [it, inserted] = streams_.try_emplace(key);
        if (created) {
            *created = inserted;
        }
        if (inserted) {
            it->second.instrumnet_id = std::string(sym);
            it->second.duration_ns   = dur;
            it->second.capacity      = cap;
        }
        return it->second;
    }

    KlineStream* find(std::string_view sym, int64_t dur) noexcept {
        auto it = streams_.find(make_key(sym, dur));
        return it == streams_.end() ? nullptr : &it->second;
    }

    void clear_changes() noexcept {
        for (auto& [_, s] : streams_) {
            s.changed_fields = 0;
        }
    }

    std::size_t size() const noexcept { return streams_.size(); }
    bool        empty() const noexcept { return streams_.empty(); }

    auto begin() noexcept { return streams_.begin(); }
    auto end() noexcept { return streams_.end(); }
    auto begin() const noexcept { return streams_.begin(); }
    auto end() const noexcept { return streams_.end(); }

private:
    std::unordered_map<std::string, KlineStream> streams_;
};

struct DataManager {
    EntityStore<Quote>    quotes;    // key: instrument_id
    EntityStore<Order>    orders;    // key: order_id
    EntityStore<Trade>    trades;    // key: trade_id
    EntityStore<Position> positions; // key: instrument_id
    Account               account;   // 单例,值类型(未收到时全默认值)
    KlineRegistry         klines;

    void clear_all_changes() noexcept {
        quotes.clear_changes();
        orders.clear_changes();
        trades.clear_changes();
        positions.clear_changes();
        klines.clear_changes();
        account.changed_fields = 0;
    }

    /// 交易状态对账前清空交易实体（行情/K 线保留）。
    void clear_trading() noexcept {
        orders.clear();
        trades.clear();
        positions.clear();
        account = Account{};
    }
};

} // namespace tqsdk
