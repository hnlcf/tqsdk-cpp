#pragma once

#include "tqsdk/data/data_series.h"
#include "tqsdk/domain/quote.h"
#include "tqsdk/domain/tick.h"

namespace tqsdk {

/// 单个合约的 tick 窗口。
///
/// tick 由 quote 快照固化而来；同一快照重复到达时不会产生重复元素。
struct TickSeries {
    InstrumentId instrument_id;
    std::uint64_t capacity = 200;
    DataSeries<Tick> data{200};

    /// 用于去重的上一份 quote 指纹。
    Datetime last_datetime;
    double   last_price  = kUnknownDouble;
    double   last_bid1   = kUnknownDouble;
    double   last_ask1   = kUnknownDouble;
    std::uint64_t last_volume = 0;

    explicit TickSeries(std::string symbol = {}, std::size_t cap = 200)
        : instrument_id(std::move(symbol)), capacity(cap), data(cap) {}

    bool append_quote(const Quote& q) {
        if (!std::isfinite(q.last_price)) {
            return false;
        }
        const bool same = q.datetime == last_datetime && q.last_price == last_price &&
                          q.bid_price1 == last_bid1 && q.ask_price1 == last_ask1 &&
                          q.volume == last_volume;
        if (same) {
            return false;
        }
        data.append(tick_from_quote(q));
        last_datetime = q.datetime;
        last_price    = q.last_price;
        last_bid1     = q.bid_price1;
        last_ask1     = q.ask_price1;
        last_volume   = q.volume;
        return true;
    }

    void reset(std::size_t cap) {
        capacity = cap;
        data.reset(cap);
        last_datetime.clear();
        last_price = kUnknownDouble;
        last_bid1  = kUnknownDouble;
        last_ask1  = kUnknownDouble;
        last_volume = 0;
    }
};

} // namespace tqsdk
