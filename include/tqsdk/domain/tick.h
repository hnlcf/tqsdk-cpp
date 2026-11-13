#pragma once

#include "tqsdk/domain/quote.h"
#include "tqsdk/pch.h"
#include "tqsdk/types.h"

namespace tqsdk {

/// 行情快照（tick 序列元素）。
///
/// 天勤行情协议本身以 quote diff 为原子更新；本结构在每次有效 quote 更新后
/// 固化一份快照，用于 tick 序列、下载和回测。字段与 Quote 保持稳定映射。
struct Tick {
    InstrumentId instrument_id;
    Datetime     datetime;

    double   last_price     = kUnknownDouble;
    double   highest        = kUnknownDouble;
    double   lowest         = kUnknownDouble;
    double   open           = kUnknownDouble;
    double   close          = kUnknownDouble;
    double   pre_close      = kUnknownDouble;
    double   pre_settlement = kUnknownDouble;
    double   average        = kUnknownDouble;
    double   turnover       = kUnknownDouble;
    double   open_interest  = kUnknownDouble;
    uint64_t volume         = 0;

    double bid_price[5] = {
        kUnknownDouble, kUnknownDouble, kUnknownDouble, kUnknownDouble, kUnknownDouble};
    double ask_price[5] = {
        kUnknownDouble, kUnknownDouble, kUnknownDouble, kUnknownDouble, kUnknownDouble};
    uint64_t bid_volume[5] = {0, 0, 0, 0, 0};
    uint64_t ask_volume[5] = {0, 0, 0, 0, 0};
};

inline Tick tick_from_quote(const Quote& q) {
    Tick t;
    t.instrument_id  = q.instrument_id;
    t.datetime       = q.datetime;
    t.last_price     = q.last_price;
    t.highest        = q.highest;
    t.lowest         = q.lowest;
    t.open           = q.open;
    t.close          = q.close;
    t.pre_close      = q.pre_close;
    t.pre_settlement = q.pre_settlement;
    t.average        = q.average;
    t.turnover       = q.turnover;
    t.open_interest  = q.open_interest;
    t.volume         = q.volume;
    t.bid_price[0]   = q.bid_price1;
    t.bid_price[1]   = q.bid_price2;
    t.bid_price[2]   = q.bid_price3;
    t.bid_price[3]   = q.bid_price4;
    t.bid_price[4]   = q.bid_price5;
    t.ask_price[0]   = q.ask_price1;
    t.ask_price[1]   = q.ask_price2;
    t.ask_price[2]   = q.ask_price3;
    t.ask_price[3]   = q.ask_price4;
    t.ask_price[4]   = q.ask_price5;
    t.bid_volume[0]  = q.bid_volume1;
    t.bid_volume[1]  = q.bid_volume2;
    t.bid_volume[2]  = q.bid_volume3;
    t.bid_volume[3]  = q.bid_volume4;
    t.bid_volume[4]  = q.bid_volume5;
    t.ask_volume[0]  = q.ask_volume1;
    t.ask_volume[1]  = q.ask_volume2;
    t.ask_volume[2]  = q.ask_volume3;
    t.ask_volume[3]  = q.ask_volume4;
    t.ask_volume[4]  = q.ask_volume5;
    return t;
}

inline bool tick_is_valid(const Tick& t) {
    return std::isfinite(t.last_price);
}

inline void to_json(nlohmann::json& j, const Tick& t) {
    auto prices = [](const double (&values)[5]) {
        nlohmann::json out = nlohmann::json::array();
        for (double v : values) {
            out.push_back(std::isfinite(v) ? nlohmann::json(v) : nullptr);
        }
        return out;
    };
    auto volumes = [](const uint64_t (&values)[5]) {
        nlohmann::json out = nlohmann::json::array();
        for (auto v : values) {
            out.push_back(v);
        }
        return out;
    };
    auto num = [](double v) { return std::isfinite(v) ? nlohmann::json(v) : nullptr; };
    j        = nlohmann::json{
        {"instrument_id", t.instrument_id},
        {"datetime", t.datetime},
        {"last_price", num(t.last_price)},
        {"highest", num(t.highest)},
        {"lowest", num(t.lowest)},
        {"open", num(t.open)},
        {"close", num(t.close)},
        {"pre_close", num(t.pre_close)},
        {"pre_settlement", num(t.pre_settlement)},
        {"average", num(t.average)},
        {"turnover", num(t.turnover)},
        {"open_interest", num(t.open_interest)},
        {"volume", t.volume},
        {"bid_price", prices(t.bid_price)},
        {"ask_price", prices(t.ask_price)},
        {"bid_volume", volumes(t.bid_volume)},
        {"ask_volume", volumes(t.ask_volume)},
    };
}

inline void from_json(const nlohmann::json& j, Tick& t) {
    auto num = [&](const char* key) -> double {
        auto it = j.find(key);
        if (it == j.end() || it->is_null()) {
            return kUnknownDouble;
        }
        return it->is_number() ? it->get<double>() : kUnknownDouble;
    };
    auto u64 = [&](const char* key) -> uint64_t {
        auto it = j.find(key);
        if (it == j.end() || it->is_null()) {
            return 0;
        }
        return it->is_number_unsigned() || it->is_number_integer() ? it->get<uint64_t>() : 0;
    };
    auto prices = [&](const char* key, double (&out)[5]) {
        auto it = j.find(key);
        if (it == j.end() || !it->is_array()) {
            return;
        }
        for (std::size_t i = 0; i < 5 && i < it->size(); ++i) {
            const auto& v = (*it)[i];
            out[i]        = v.is_null() || !v.is_number() ? kUnknownDouble : v.get<double>();
        }
    };
    auto volumes = [&](const char* key, uint64_t (&out)[5]) {
        auto it = j.find(key);
        if (it == j.end() || !it->is_array()) {
            return;
        }
        for (std::size_t i = 0; i < 5 && i < it->size(); ++i) {
            const auto& v = (*it)[i];
            out[i]        = v.is_number_unsigned() || v.is_number_integer() ? v.get<uint64_t>() : 0;
        }
    };

    t.instrument_id  = j.value("instrument_id", std::string{});
    t.datetime       = j.value("datetime", std::string{});
    t.last_price     = num("last_price");
    t.highest        = num("highest");
    t.lowest         = num("lowest");
    t.open           = num("open");
    t.close          = num("close");
    t.pre_close      = num("pre_close");
    t.pre_settlement = num("pre_settlement");
    t.average        = num("average");
    t.turnover       = num("turnover");
    t.open_interest  = num("open_interest");
    t.volume         = u64("volume");
    prices("bid_price", t.bid_price);
    prices("ask_price", t.ask_price);
    volumes("bid_volume", t.bid_volume);
    volumes("ask_volume", t.ask_volume);
}

} // namespace tqsdk
