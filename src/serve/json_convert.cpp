#include "tqsdk/serve/json_convert.h"

#include "tqsdk/instrument/instrument_catalog.h"

#include "tqsdk/domain/enum_util.h"

namespace tqsdk {
namespace serve {

namespace {

using json = nlohmann::json;

json jnum(double v) {
    return std::isfinite(v) ? json(v) : json(nullptr);
}

template <typename E>
json enum_str(E e) {
    return json(std::string(magic_enum::enum_name(e)));
}

json bar_to_json(const KlineBar& b) {
    return json{{"datetime", b.datetime},
                {"open", jnum(b.open)},
                {"high", jnum(b.high)},
                {"low", jnum(b.low)},
                {"close", jnum(b.close)},
                {"volume", b.volume},
                {"open_oi", b.open_oi},
                {"close_oi", b.close_oi}};
}

// 按 bits 填充 Quote；bits == ~0ull 时等价于全量快照。
void fill_quote(json& j, const Quote& q, uint64_t bits) {
#define TQ_QUOTE_FIELD(field, name, value)                                                         \
    if (bits & static_cast<uint64_t>(QuoteField::field)) {                                         \
        j[name] = (value);                                                                         \
    }

    TQ_QUOTE_FIELD(datetime, "datetime", q.datetime)
    TQ_QUOTE_FIELD(volume_multiple, "volume_multiple", q.volume_multiple)
    TQ_QUOTE_FIELD(price_tick, "price_tick", jnum(q.price_tick))
    TQ_QUOTE_FIELD(price_decs, "price_decs", q.price_decs)
    TQ_QUOTE_FIELD(max_market_order_volume, "max_market_order_volume", q.max_market_order_volume)
    TQ_QUOTE_FIELD(min_market_order_volume, "min_market_order_volume", q.min_market_order_volume)
    TQ_QUOTE_FIELD(max_limit_order_volume, "max_limit_order_volume", q.max_limit_order_volume)
    TQ_QUOTE_FIELD(min_limit_order_volume, "min_limit_order_volume", q.min_limit_order_volume)
    TQ_QUOTE_FIELD(margin, "margin", jnum(q.margin))
    TQ_QUOTE_FIELD(commission, "commission", jnum(q.commission))

    TQ_QUOTE_FIELD(last_price, "last_price", jnum(q.last_price))
    TQ_QUOTE_FIELD(pre_settlement, "pre_settlement", jnum(q.pre_settlement))
    TQ_QUOTE_FIELD(pre_close, "pre_close", jnum(q.pre_close))
    TQ_QUOTE_FIELD(pre_open_interest, "pre_open_interest", jnum(q.pre_open_interest))
    TQ_QUOTE_FIELD(open, "open", jnum(q.open))
    TQ_QUOTE_FIELD(highest, "highest", jnum(q.highest))
    TQ_QUOTE_FIELD(lowest, "lowest", jnum(q.lowest))
    TQ_QUOTE_FIELD(volume, "volume", q.volume)
    TQ_QUOTE_FIELD(turnover, "turnover", jnum(q.turnover))
    TQ_QUOTE_FIELD(open_interest, "open_interest", jnum(q.open_interest))
    TQ_QUOTE_FIELD(close, "close", jnum(q.close))
    TQ_QUOTE_FIELD(average, "average", jnum(q.average))
    TQ_QUOTE_FIELD(settlement, "settlement", jnum(q.settlement))
    TQ_QUOTE_FIELD(upper_limit, "upper_limit", jnum(q.upper_limit))
    TQ_QUOTE_FIELD(lower_limit, "lower_limit", jnum(q.lower_limit))

    TQ_QUOTE_FIELD(bid_price1, "bid_price1", jnum(q.bid_price1))
    TQ_QUOTE_FIELD(ask_price1, "ask_price1", jnum(q.ask_price1))
    TQ_QUOTE_FIELD(bid_price2, "bid_price2", jnum(q.bid_price2))
    TQ_QUOTE_FIELD(ask_price2, "ask_price2", jnum(q.ask_price2))
    TQ_QUOTE_FIELD(bid_price3, "bid_price3", jnum(q.bid_price3))
    TQ_QUOTE_FIELD(ask_price3, "ask_price3", jnum(q.ask_price3))
    TQ_QUOTE_FIELD(bid_price4, "bid_price4", jnum(q.bid_price4))
    TQ_QUOTE_FIELD(ask_price4, "ask_price4", jnum(q.ask_price4))
    TQ_QUOTE_FIELD(bid_price5, "bid_price5", jnum(q.bid_price5))
    TQ_QUOTE_FIELD(ask_price5, "ask_price5", jnum(q.ask_price5))

    TQ_QUOTE_FIELD(bid_volume1, "bid_volume1", q.bid_volume1)
    TQ_QUOTE_FIELD(ask_volume1, "ask_volume1", q.ask_volume1)
    TQ_QUOTE_FIELD(bid_volume2, "bid_volume2", q.bid_volume2)
    TQ_QUOTE_FIELD(ask_volume2, "ask_volume2", q.ask_volume2)
    TQ_QUOTE_FIELD(bid_volume3, "bid_volume3", q.bid_volume3)
    TQ_QUOTE_FIELD(ask_volume3, "ask_volume3", q.ask_volume3)
    TQ_QUOTE_FIELD(bid_volume4, "bid_volume4", q.bid_volume4)
    TQ_QUOTE_FIELD(ask_volume4, "ask_volume4", q.ask_volume4)
    TQ_QUOTE_FIELD(bid_volume5, "bid_volume5", q.bid_volume5)
    TQ_QUOTE_FIELD(ask_volume5, "ask_volume5", q.ask_volume5)

#undef TQ_QUOTE_FIELD
}

} // namespace

namespace {

void fill_quote_static(json& j, const Quote& q, const InstrumentCatalog* catalog) {
    if (catalog == nullptr || q.instrument_id.empty()) {
        return;
    }
    const auto instrument = catalog->find(q.instrument_id);
    if (!instrument) {
        return;
    }
    if (!j.contains("price_tick") || j["price_tick"].is_null()) {
        j["price_tick"] = instrument->price_tick;
    }
    // 目录里的静态规格比交易所推送更完整；price_decs 甚至可能被默认成 0。
    j["volume_multiple"] = static_cast<uint64_t>(instrument->volume_multiple);
    j["price_decs"]      = static_cast<uint64_t>(instrument->price_decs);
    j["instrument_name"] = instrument->name;
}

} // namespace

nlohmann::json quote_to_json(const Quote& q, const InstrumentCatalog* catalog) {
    json j             = json::object();
    j["instrument_id"] = q.instrument_id;
    fill_quote(j, q, ~uint64_t{0});
    fill_quote_static(j, q, catalog);
    return j;
}

nlohmann::json quote_diff_to_json(const Quote& q, const InstrumentCatalog* catalog) {
    json j = json::object();
    fill_quote(j, q, q.changed_fields);
    fill_quote_static(j, q, catalog);
    return j;
}

nlohmann::json kline_to_json(const KlineStream& s) {
    json j;
    j["symbol"]      = s.instrumnet_id;
    j["duration_ns"] = s.duration_ns;
    j["bars"]        = json::array();
    for (const auto& b : s.bars) {
        j["bars"].push_back(bar_to_json(b));
    }
    return j;
}

nlohmann::json kline_ws_json(const KlineStream& s, bool force_reset) {
    const bool reset = force_reset || (s.changed_fields & KlineChangeDetail::Reset) != 0;
    json       node;
    node["reset"] = reset;
    node["bars"]  = json::array();
    if (reset) {
        for (const auto& b : s.bars) {
            node["bars"].push_back(bar_to_json(b));
        }
    } else if (!s.bars.empty()) {
        node["bars"].push_back(bar_to_json(s.bars.back()));
    }

    json out;
    out["klines"][s.instrumnet_id][std::to_string(s.duration_ns)] = std::move(node);
    return out;
}

nlohmann::json account_to_json(const Account& a) {
    json j;
    j["currency"]          = enum_str(a.currency);
    j["pre_balance"]       = jnum(a.pre_balance);
    j["deposit"]           = jnum(a.deposit);
    j["withdraw"]          = jnum(a.withdraw);
    j["static_balance"]    = jnum(a.static_balance);
    j["close_profit"]      = jnum(a.close_profit);
    j["commission"]        = jnum(a.commission);
    j["premium"]           = jnum(a.premium);
    j["position_profit"]   = jnum(a.position_profit);
    j["float_profit"]      = jnum(a.float_profit);
    j["balance"]           = jnum(a.balance);
    j["margin"]            = jnum(a.margin);
    j["frozen_margin"]     = jnum(a.frozen_margin);
    j["frozen_commission"] = jnum(a.frozen_commission);
    j["frozen_premium"]    = jnum(a.frozen_premium);
    j["available"]         = jnum(a.available);
    j["risk_ratio"]        = jnum(a.risk_ratio);
    return j;
}

nlohmann::json order_to_json(const Order& o) {
    json j;
    j["order_id"]          = o.order_id;
    j["order_type"]        = enum_str(o.order_type);
    j["instrument_id"]     = o.instrument_id;
    j["exchange_id"]       = o.exchange_id;
    j["direction"]         = enum_str(o.direction);
    j["offset"]            = enum_str(o.offset);
    j["volume_orign"]      = o.volume_orign;
    j["volume_left"]       = o.volume_left;
    j["trade_type"]        = enum_str(o.trade_type);
    j["price_type"]        = enum_str(o.price_type);
    j["limit_price"]       = jnum(o.limit_price);
    j["time_cond"]         = enum_str(o.time_cond);
    j["volume_cond"]       = enum_str(o.volume_cond);
    j["min_volume"]        = o.min_volume;
    j["hedge_flag"]        = o.hedge_flag;
    j["status"]            = enum_str(o.status);
    j["last_msg"]          = o.last_msg;
    j["insert_date_time"]  = o.insert_date_time;
    j["exchange_order_id"] = o.exchange_order_id;
    j["trade_price"]       = jnum(o.trade_price);
    j["is_error"]          = o.is_error;
    j["is_dead"]           = o.is_dead;
    return j;
}

nlohmann::json trade_to_json(const Trade& t) {
    json j;
    j["trade_id"]          = t.trade_id;
    j["order_id"]          = t.order_id;
    j["exchange_id"]       = t.exchange_id;
    j["instrument_id"]     = t.instrument_id;
    j["exchange_trade_id"] = t.exchange_trade_id;
    j["direction"]         = enum_str(t.direction);
    j["offset"]            = enum_str(t.offset);
    j["volume"]            = t.volume;
    j["price"]             = jnum(t.price);
    j["trade_date_time"]   = t.trade_date_time;
    j["commission"]        = jnum(t.commission);
    return j;
}

nlohmann::json position_to_json(const Position& p) {
    json j;
    j["instrument_id"]             = p.instrument_id;
    j["exchange_id"]               = p.exchange_id;
    j["volume_long_today"]         = p.volume_long_today;
    j["volume_long_his"]           = p.volume_long_his;
    j["volume_long"]               = p.volume_long;
    j["volume_long_frozen_today"]  = p.volume_long_frozen_today;
    j["volume_long_frozen_his"]    = p.volume_long_frozen_his;
    j["volume_short_today"]        = p.volume_short_today;
    j["volume_short_his"]          = p.volume_short_his;
    j["volume_short"]              = p.volume_short;
    j["volume_short_frozen_today"] = p.volume_short_frozen_today;
    j["volume_short_frozen_his"]   = p.volume_short_frozen_his;
    j["order_volume_buy_open"]     = p.order_volume_buy_open;
    j["order_volume_buy_close"]    = p.order_volume_buy_close;
    j["order_volume_sell_open"]    = p.order_volume_sell_open;
    j["order_volume_sell_close"]   = p.order_volume_sell_close;
    j["open_price_long"]           = jnum(p.open_price_long);
    j["open_price_short"]          = jnum(p.open_price_short);
    j["open_cost_long"]            = jnum(p.open_cost_long);
    j["open_cost_short"]           = jnum(p.open_cost_short);
    j["position_price_long"]       = jnum(p.position_price_long);
    j["position_price_short"]      = jnum(p.position_price_short);
    j["position_cost_long"]        = jnum(p.position_cost_long);
    j["position_cost_short"]       = jnum(p.position_cost_short);
    j["last_price"]                = jnum(p.last_price);
    j["float_profit_long"]         = jnum(p.float_profit_long);
    j["float_profit_short"]        = jnum(p.float_profit_short);
    j["float_profit"]              = jnum(p.float_profit);
    j["position_profit_long"]      = jnum(p.position_profit_long);
    j["position_profit_short"]     = jnum(p.position_profit_short);
    j["position_profit"]           = jnum(p.position_profit);
    j["margin_long"]               = jnum(p.margin_long);
    j["margin_short"]              = jnum(p.margin_short);
    j["margin"]                    = jnum(p.margin);
    return j;
}

} // namespace serve
} // namespace tqsdk
