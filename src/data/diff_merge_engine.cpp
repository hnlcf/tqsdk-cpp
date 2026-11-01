#include "tqsdk/data/diff_merge_engine.h"
#include "tqsdk/data/field_table.h"
#include "tqsdk/domain/enum_util.h"
#include "tqsdk/types.h"

namespace tqsdk {

void DiffMergeEngine::apply(const nlohmann::json& push) {
    if (!push.is_object())
        return;
    for (auto it = push.begin(); it != push.end(); ++it) {
        std::string_view k = it.key();
        if (k == "quotes") {
            merge_quotes(it.value());
        } else if (k == "trade") {
            merge_trade(it.value());
        } else if (k == "klines") {
            merge_klines(it.value());
        } else {
            // 未知顶层路由忽略
            spdlog::error("Unimplemented route for merging: {}", k);
            continue;
        }
    }
}

void DiffMergeEngine::merge_quotes(const nlohmann::json& quotes) {
    if (!quotes.is_object()) {
        return;
    }
    for (auto it = quotes.begin(); it != quotes.end(); ++it) {
        merge_one_quote(it.key(), it.value());
    }
}

#define MERGE_STR(F)                                                                               \
    case QuoteField::F: {                                                                          \
        if (v.is_string()) {                                                                       \
            q.F = v.get<std::string>();                                                            \
            bits |= (uint64_t)f;                                                                   \
        }                                                                                          \
        break;                                                                                     \
    }

#define MERGE_UINT(F)                                                                              \
    case QuoteField::F: {                                                                          \
        if (v.is_number_unsigned()) {                                                              \
            q.F = v.get<uint64_t>();                                                               \
            bits |= (uint64_t)f;                                                                   \
        } else if (v.is_number_integer() && v.get<int64_t>() >= 0) {                               \
            q.F = static_cast<uint64_t>(v.get<int64_t>());                                         \
            bits |= (uint64_t)f;                                                                   \
        }                                                                                          \
        break;                                                                                     \
    }

#define MERGE_DBL(F)                                                                               \
    case QuoteField::F: {                                                                          \
        if (v.is_number()) {                                                                       \
            q.F = v.get<double>();                                                                 \
            bits |= (uint64_t)f;                                                                   \
        }                                                                                          \
        break;                                                                                     \
    }

void DiffMergeEngine::merge_one_quote(std::string_view sym, const nlohmann::json& diff) {
    if (!diff.is_object()) {
        return;
    }

    auto& q = data_.quotes.get_or_create(std::string(sym));
    if (q.instrument_id.empty()) {
        q.instrument_id = sym;
    }

    uint64_t bits = 0;
    for (auto it = diff.begin(); it != diff.end(); ++it) {
        const auto& v = it.value();
        auto        f = lookup_quote_field(it.key());
        if (f == QuoteField{0}) {
            continue;
        }

        switch (f) {
            MERGE_STR(datetime)
            MERGE_UINT(volume_multiple)
            MERGE_UINT(price_decs)
            MERGE_UINT(max_market_order_volume)
            MERGE_UINT(min_market_order_volume)
            MERGE_UINT(max_limit_order_volume)
            MERGE_UINT(min_limit_order_volume)
            MERGE_UINT(volume)
            MERGE_UINT(bid_volume1)
            MERGE_UINT(ask_volume1)
            MERGE_UINT(bid_volume2)
            MERGE_UINT(ask_volume2)
            MERGE_UINT(bid_volume3)
            MERGE_UINT(ask_volume3)
            MERGE_UINT(bid_volume4)
            MERGE_UINT(ask_volume4)
            MERGE_UINT(bid_volume5)
            MERGE_UINT(ask_volume5)
            MERGE_DBL(price_tick)
            MERGE_DBL(margin)
            MERGE_DBL(commission)
            MERGE_DBL(last_price)
            MERGE_DBL(pre_settlement)
            MERGE_DBL(pre_close)
            MERGE_DBL(pre_open_interest)
            MERGE_DBL(open)
            MERGE_DBL(highest)
            MERGE_DBL(lowest)
            MERGE_DBL(turnover)
            MERGE_DBL(open_interest)
            MERGE_DBL(close)
            MERGE_DBL(average)
            MERGE_DBL(settlement)
            MERGE_DBL(upper_limit)
            MERGE_DBL(lower_limit)
            MERGE_DBL(bid_price1)
            MERGE_DBL(ask_price1)
            MERGE_DBL(bid_price2)
            MERGE_DBL(ask_price2)
            MERGE_DBL(bid_price3)
            MERGE_DBL(ask_price3)
            MERGE_DBL(bid_price4)
            MERGE_DBL(ask_price4)
            MERGE_DBL(bid_price5)
            MERGE_DBL(ask_price5)
        default:
            break;
        }
    }
    if (bits)
        data_.quotes.mark_changed(q, bits);
}

void DiffMergeEngine::merge_trade(const nlohmann::json& trade) {
    if (!trade.is_object()) {
        return;
    }

    for (auto it = trade.begin(); it != trade.end(); ++it) {
        const std::string& outer_key = it.key(); // 可能是 "" 或 symbol
        const auto&        node      = it.value();
        if (!node.is_object()) {
            continue;
        }

        auto user_it = node.find("user");
        if (user_it == node.end() || !user_it->is_object()) {
            continue;
        }
        const auto& user = *user_it;

        // ---- orders: key = order_id ----
        if (auto p = user.find("orders"); p != user.end() && p->is_object()) {
            for (auto oit = p->begin(); oit != p->end(); ++oit) {
                auto& o = data_.orders.get_or_create(oit.key());
                if (o.order_id.empty())
                    o.order_id = oit.key();
                // 若外层 key 是 symbol,顺手填 instrument_id(不置位)
                if (o.instrument_id.empty() && !outer_key.empty()) {
                    o.instrument_id = outer_key;
                }
                merge_one_order(o, oit.value());
            }
        }

        // ---- trades: key = trade_id ----
        if (auto p = user.find("trades"); p != user.end() && p->is_object()) {
            for (auto tit = p->begin(); tit != p->end(); ++tit) {
                auto& t = data_.trades.get_or_create(tit.key());
                if (t.trade_id.empty()) {
                    t.trade_id = tit.key();
                }
                if (t.instrument_id.empty() && !outer_key.empty()) {
                    t.instrument_id = outer_key;
                }
                merge_one_trade(t, tit.value());
            }
        }

        // ---- positions: key = instrument_id ----
        if (auto p = user.find("positions"); p != user.end() && p->is_object()) {
            for (auto pit = p->begin(); pit != p->end(); ++pit) {
                auto& pos = data_.positions.get_or_create(pit.key());
                if (pos.instrument_id.empty()) {
                    pos.instrument_id = pit.key();
                }
                merge_one_position(pos, pit.value());
            }
        }

        // ---- accounts: key = currency(第一版只处理 CNY) ----
        if (auto p = user.find("accounts"); p != user.end() && p->is_object()) {
            if (auto cny = p->find("CNY"); cny != p->end()) {
                merge_one_account(data_.account, *cny);
            }
        }
    }
}

void DiffMergeEngine::merge_one_order(Order& o, const nlohmann::json& diff) {
    if (!diff.is_object()) {
        return;
    }

    uint64_t bits = 0;
    for (auto it = diff.begin(); it != diff.end(); ++it) {
        const auto& v = it.value();
        auto        f = lookup_order_field(it.key());
        if (f == OrderField{0}) {
            continue;
        }

        switch (f) {
        // ---- 枚举:走 try_get_enum,失败静默跳过 ----
        case OrderField::direction:
            if (auto e = try_get_enum<Direction>(v)) {
                o.direction = *e;
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::offset:
            if (auto e = try_get_enum<Offset>(v)) {
                o.offset = *e;
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::status:
            if (auto e = try_get_enum<OrderStatus>(v)) {
                o.status = *e;
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::order_type:
            if (auto e = try_get_enum<OrderType>(v)) {
                o.order_type = *e;
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::price_type:
            if (auto e = try_get_enum<PriceType>(v)) {
                o.price_type = *e;
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::trade_type:
            if (auto e = try_get_enum<TradeType>(v)) {
                o.trade_type = *e;
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::time_cond:
            if (auto e = try_get_enum<TimeCondition>(v)) {
                o.time_cond = *e;
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::volume_cond:
            if (auto e = try_get_enum<VolumeCondition>(v)) {
                o.volume_cond = *e;
                bits |= (uint64_t)f;
            }
            break;

        // ---- 数值 ----
        case OrderField::volume_orign:
            if (v.is_number_integer() && v.get<int64_t>() >= 0) {
                o.volume_orign = static_cast<uint64_t>(v.get<int64_t>());
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::volume_left:
            if (v.is_number_integer() && v.get<int64_t>() >= 0) {
                o.volume_left = static_cast<uint64_t>(v.get<int64_t>());
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::min_volume:
            if (v.is_number_integer() && v.get<int64_t>() >= 0) {
                o.min_volume = static_cast<uint64_t>(v.get<int64_t>());
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::limit_price:
            if (v.is_number()) {
                o.limit_price = v.get<double>();
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::trade_price:
            if (v.is_number()) {
                o.trade_price = v.get<double>();
                bits |= (uint64_t)f;
            }
            break;

        // ---- bool ----
        case OrderField::is_error:
            if (v.is_boolean()) {
                o.is_error = v.get<bool>();
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::is_dead:
            if (v.is_boolean()) {
                o.is_dead = v.get<bool>();
                bits |= (uint64_t)f;
            }
            break;

        // ---- 字符串 ----
        case OrderField::exchange_id:
            if (v.is_string()) {
                o.exchange_id = v.get<std::string>();
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::instrument_id:
            if (v.is_string()) {
                o.instrument_id = v.get<std::string>();
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::hedge_flag:
            if (v.is_string()) {
                o.hedge_flag = v.get<std::string>();
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::last_msg:
            if (v.is_string()) {
                o.last_msg = v.get<std::string>();
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::exchange_order_id:
            if (v.is_string()) {
                o.exchange_order_id = v.get<std::string>();
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::insert_date_time:
            // TimeStamp = uint64_t
            if (v.is_number_unsigned()) {
                o.insert_date_time = v.get<uint64_t>();
                bits |= (uint64_t)f;
            } else if (v.is_number_integer() && v.get<int64_t>() >= 0) {
                o.insert_date_time = static_cast<uint64_t>(v.get<int64_t>());
                bits |= (uint64_t)f;
            }
            break;

        default:
            break;
        }
    }
    if (bits) {
        data_.orders.mark_changed(o, bits);
    }
}

void DiffMergeEngine::merge_one_trade(Trade& t, const nlohmann::json& diff) {
    if (!diff.is_object()) {
        return;
    }
    uint64_t bits = 0;
    for (auto it = diff.begin(); it != diff.end(); ++it) {
        const auto& v = it.value();
        auto        f = lookup_trade_field(it.key());
        if (f == TradeField{0}) {
            continue;
        }

        switch (f) {
        case TradeField::direction:
            if (auto e = try_get_enum<Direction>(v)) {
                t.direction = *e;
                bits |= (uint64_t)f;
            }
            break;
        case TradeField::offset:
            if (auto e = try_get_enum<Offset>(v)) {
                t.offset = *e;
                bits |= (uint64_t)f;
            }
            break;
        case TradeField::volume:
            if (v.is_number_integer() && v.get<int64_t>() >= 0) {
                t.volume = static_cast<uint64_t>(v.get<int64_t>());
                bits |= (uint64_t)f;
            }
            break;
        case TradeField::price:
            if (v.is_number()) {
                t.price = v.get<double>();
                bits |= (uint64_t)f;
            }
            break;
        case TradeField::commission:
            if (v.is_number()) {
                t.commission = v.get<double>();
                bits |= (uint64_t)f;
            }
            break;
        case TradeField::order_id:
            if (v.is_string()) {
                t.order_id = v.get<std::string>();
                bits |= (uint64_t)f;
            }
            break;
        case TradeField::exchange_id:
            if (v.is_string()) {
                t.exchange_id = v.get<std::string>();
                bits |= (uint64_t)f;
            }
            break;
        case TradeField::instrument_id:
            if (v.is_string()) {
                t.instrument_id = v.get<std::string>();
                bits |= (uint64_t)f;
            }
            break;
        case TradeField::exchange_trade_id:
            if (v.is_string()) {
                t.exchange_trade_id = v.get<std::string>();
                bits |= (uint64_t)f;
            }
            break;
        case TradeField::trade_date_time:
            if (v.is_number_unsigned()) {
                t.trade_date_time = v.get<uint64_t>();
                bits |= (uint64_t)f;
            } else if (v.is_number_integer() && v.get<int64_t>() >= 0) {
                t.trade_date_time = static_cast<uint64_t>(v.get<int64_t>());
                bits |= (uint64_t)f;
            }
            break;
        default:
            break;
        }
    }
    if (bits)
        data_.trades.mark_changed(t, bits);
}

void DiffMergeEngine::merge_one_position(Position& p, const nlohmann::json& diff) {
    if (!diff.is_object()) {
        return;
    }
    uint64_t bits = 0;
    for (auto it = diff.begin(); it != diff.end(); ++it) {
        const auto& v = it.value();
        auto        f = lookup_position_field(it.key());
        if (f == PositionField{0}) {
            continue;
        }

        switch (f) {
        case PositionField::exchange_id:
            if (v.is_string()) {
                p.exchange_id = v.get<std::string>();
                bits |= (uint64_t)f;
            }
            break;

        // 全部 volume_* / order_volume_* 都是 uint64
        case PositionField::volume_long:
        case PositionField::volume_long_today:
        case PositionField::volume_long_his:
        case PositionField::volume_long_frozen_today:
        case PositionField::volume_long_frozen_his:
        case PositionField::volume_short:
        case PositionField::volume_short_today:
        case PositionField::volume_short_his:
        case PositionField::volume_short_frozen_today:
        case PositionField::volume_short_frozen_his:
        case PositionField::order_volume_buy_open:
        case PositionField::order_volume_buy_close:
        case PositionField::order_volume_sell_open:
        case PositionField::order_volume_sell_close: {
            if (!v.is_number_integer() || v.get<int64_t>() < 0)
                break;
            uint64_t n = static_cast<uint64_t>(v.get<int64_t>());
            // 按字段分派到具体成员(用 f 判定)
            if (f == PositionField::volume_long)
                p.volume_long = n;
            else if (f == PositionField::volume_long_today)
                p.volume_long_today = n;
            else if (f == PositionField::volume_long_his)
                p.volume_long_his = n;
            else if (f == PositionField::volume_long_frozen_today)
                p.volume_long_frozen_today = n;
            else if (f == PositionField::volume_long_frozen_his)
                p.volume_long_frozen_his = n;
            else if (f == PositionField::volume_short)
                p.volume_short = n;
            else if (f == PositionField::volume_short_today)
                p.volume_short_today = n;
            else if (f == PositionField::volume_short_his)
                p.volume_short_his = n;
            else if (f == PositionField::volume_short_frozen_today)
                p.volume_short_frozen_today = n;
            else if (f == PositionField::volume_short_frozen_his)
                p.volume_short_frozen_his = n;
            else if (f == PositionField::order_volume_buy_open)
                p.order_volume_buy_open = n;
            else if (f == PositionField::order_volume_buy_close)
                p.order_volume_buy_close = n;
            else if (f == PositionField::order_volume_sell_open)
                p.order_volume_sell_open = n;
            else if (f == PositionField::order_volume_sell_close)
                p.order_volume_sell_close = n;
            bits |= (uint64_t)f;
            break;
        }

        // 全部价格 / 盈亏 / 保证金都是 double
        case PositionField::open_price_long:
        case PositionField::open_price_short:
        case PositionField::open_cost_long:
        case PositionField::open_cost_short:
        case PositionField::position_price_long:
        case PositionField::position_price_short:
        case PositionField::position_cost_long:
        case PositionField::position_cost_short:
        case PositionField::last_price:
        case PositionField::float_profit_long:
        case PositionField::float_profit_short:
        case PositionField::float_profit:
        case PositionField::position_profit_long:
        case PositionField::position_profit_short:
        case PositionField::position_profit:
        case PositionField::margin_long:
        case PositionField::margin_short:
        case PositionField::margin: {
            if (!v.is_number())
                break;
            double d = v.get<double>();
            if (f == PositionField::open_price_long)
                p.open_price_long = d;
            else if (f == PositionField::open_price_short)
                p.open_price_short = d;
            else if (f == PositionField::open_cost_long)
                p.open_cost_long = d;
            else if (f == PositionField::open_cost_short)
                p.open_cost_short = d;
            else if (f == PositionField::position_price_long)
                p.position_price_long = d;
            else if (f == PositionField::position_price_short)
                p.position_price_short = d;
            else if (f == PositionField::position_cost_long)
                p.position_cost_long = d;
            else if (f == PositionField::position_cost_short)
                p.position_cost_short = d;
            else if (f == PositionField::last_price)
                p.last_price = d;
            else if (f == PositionField::float_profit_long)
                p.float_profit_long = d;
            else if (f == PositionField::float_profit_short)
                p.float_profit_short = d;
            else if (f == PositionField::float_profit)
                p.float_profit = d;
            else if (f == PositionField::position_profit_long)
                p.position_profit_long = d;
            else if (f == PositionField::position_profit_short)
                p.position_profit_short = d;
            else if (f == PositionField::position_profit)
                p.position_profit = d;
            else if (f == PositionField::margin_long)
                p.margin_long = d;
            else if (f == PositionField::margin_short)
                p.margin_short = d;
            else if (f == PositionField::margin)
                p.margin = d;
            bits |= (uint64_t)f;
            break;
        }
        default:
            break;
        }
    }
    if (bits) {
        data_.positions.mark_changed(p, bits);
    }
}

void DiffMergeEngine::merge_one_account(Account& a, const nlohmann::json& diff) {
    if (!diff.is_object()) {
        return;
    }
    uint64_t bits = 0;
    for (auto it = diff.begin(); it != diff.end(); ++it) {
        const auto& v = it.value();
        auto        f = lookup_account_field(it.key());
        if (f == AccountField{0}) {
            continue;
        }

        if (f == AccountField::currency) {
            if (auto e = try_get_enum<Currency>(v)) {
                a.currency = *e;
                bits |= (uint64_t)f;
            }
            continue;
        }
        if (!v.is_number()) {
            continue;
        }

        double d = v.get<double>();
        switch (f) {
        case AccountField::pre_balance:
            a.pre_balance = d;
            bits |= (uint64_t)f;
            break;
        case AccountField::deposit:
            a.deposit = d;
            bits |= (uint64_t)f;
            break;
        case AccountField::withdraw:
            a.withdraw = d;
            bits |= (uint64_t)f;
            break;
        case AccountField::static_balance:
            a.static_balance = d;
            bits |= (uint64_t)f;
            break;
        case AccountField::close_profit:
            a.close_profit = d;
            bits |= (uint64_t)f;
            break;
        case AccountField::commission:
            a.commission = d;
            bits |= (uint64_t)f;
            break;
        case AccountField::premium:
            a.premium = d;
            bits |= (uint64_t)f;
            break;
        case AccountField::position_profit:
            a.position_profit = d;
            bits |= (uint64_t)f;
            break;
        case AccountField::float_profit:
            a.float_profit = d;
            bits |= (uint64_t)f;
            break;
        case AccountField::balance:
            a.balance = d;
            bits |= (uint64_t)f;
            break;
        case AccountField::margin:
            a.margin = d;
            bits |= (uint64_t)f;
            break;
        case AccountField::frozen_margin:
            a.frozen_margin = d;
            bits |= (uint64_t)f;
            break;
        case AccountField::frozen_commission:
            a.frozen_commission = d;
            bits |= (uint64_t)f;
            break;
        case AccountField::frozen_premium:
            a.frozen_premium = d;
            bits |= (uint64_t)f;
            break;
        case AccountField::available:
            a.available = d;
            bits |= (uint64_t)f;
            break;
        case AccountField::risk_ratio:
            a.risk_ratio = d;
            bits |= (uint64_t)f;
            break;
        default:
            break;
        }
    }
    if (bits) {
        a.changed_fields |= bits;
    }
}

void DiffMergeEngine::merge_klines(const nlohmann::json& klines) {
    if (!klines.is_object()) {
        return;
    }
    for (auto sit = klines.begin(); sit != klines.end(); ++sit) {
        const auto& dur_map = sit.value();
        if (!dur_map.is_object()) {
            continue;
        }
        for (auto dit = dur_map.begin(); dit != dur_map.end(); ++dit) {
            int64_t dur = 0;
            try {
                dur = std::stoll(dit.key());
            } catch (...) {
                continue;
            }
            merge_one_kline(sit.key(), dur, dit.value());
        }
    }
}

void DiffMergeEngine::merge_one_kline(std::string_view      sym,
                                      int64_t               dur,
                                      const nlohmann::json& bars) {
    if (!bars.is_array() || bars.empty()) {
        return;
    }

    auto& s = data_.klines.get_or_create(sym, dur, 200);

    auto parse_bar = [](const nlohmann::json& b) {
        KlineBar r;
        if (!b.is_object()) {
            return r;
        }
        auto num = [&]<typename T>(const char* k, T& dst) {
            if (auto v = b.find(k); v != b.end() && v->is_number()) {
                dst = v->get<T>();
            }
        };
        num("datetime", r.datetime);
        num("open", r.open);
        num("high", r.high);
        num("low", r.low);
        num("close", r.close);
        num("volume", r.volume);
        num("open_oi", r.open_oi);
        num("close_oi", r.close_oi);
        return r;
    };

    if (bars.size() > 1) {
        // 历史加载:整体替换
        s.bars.clear();
        for (auto& b : bars) {
            s.bars.push_back(parse_bar(b));
        }
        while (static_cast<int>(s.bars.size()) > s.capacity) {
            s.bars.pop_front();
        }
        s.changed_fields |= KlineChangeDetail::Reset;
        return;
    }

    // 单根:更新最后一根 or 追加
    KlineBar b = parse_bar(bars[0]);
    if (!s.bars.empty() && s.bars.back().datetime == b.datetime) {
        s.bars.back() = std::move(b);
        s.changed_fields |= KlineChangeDetail::LastUpdated;
    } else {
        s.bars.push_back(std::move(b));
        while (static_cast<int>(s.bars.size()) > s.capacity) {
            s.bars.pop_front();
        }
        s.changed_fields |= KlineChangeDetail::Appended;
    }
}

} // namespace tqsdk
