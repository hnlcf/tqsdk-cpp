#include "tqsdk/data/diff_merge_engine.h"
#include "tqsdk/data/field_table.h"
#include "tqsdk/domain/enum_util.h"
#include "tqsdk/types.h"

namespace tqsdk {

namespace {

bool same_double(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) {
        return true;
    }
    return a == b;
}

uint64_t order_diff_bits(const Order& before, const Order& after) {
    uint64_t bits = 0;
#define ORDER_DIFF(FIELD, EXPR)                                                                    \
    if (!((EXPR))) {                                                                               \
        bits |= static_cast<uint64_t>(OrderField::FIELD);                                          \
    }
    ORDER_DIFF(order_type, before.order_type == after.order_type)
    ORDER_DIFF(exchange_id, before.exchange_id == after.exchange_id)
    ORDER_DIFF(instrument_id, before.instrument_id == after.instrument_id)
    ORDER_DIFF(direction, before.direction == after.direction)
    ORDER_DIFF(offset, before.offset == after.offset)
    ORDER_DIFF(volume_orign, before.volume_orign == after.volume_orign)
    ORDER_DIFF(volume_left, before.volume_left == after.volume_left)
    ORDER_DIFF(trade_type, before.trade_type == after.trade_type)
    ORDER_DIFF(price_type, before.price_type == after.price_type)
    ORDER_DIFF(limit_price, same_double(before.limit_price, after.limit_price))
    ORDER_DIFF(time_cond, before.time_cond == after.time_cond)
    ORDER_DIFF(volume_cond, before.volume_cond == after.volume_cond)
    ORDER_DIFF(advanced, before.advanced == after.advanced)
    ORDER_DIFF(offset_leg2, before.offset_leg2 == after.offset_leg2)
    ORDER_DIFF(min_volume, before.min_volume == after.min_volume)
    ORDER_DIFF(hedge_flag, before.hedge_flag == after.hedge_flag)
    ORDER_DIFF(status, before.status == after.status)
    ORDER_DIFF(last_msg, before.last_msg == after.last_msg)
    ORDER_DIFF(insert_date_time, before.insert_date_time == after.insert_date_time)
    ORDER_DIFF(exchange_order_id, before.exchange_order_id == after.exchange_order_id)
    ORDER_DIFF(trade_price, same_double(before.trade_price, after.trade_price))
    ORDER_DIFF(is_error, before.is_error == after.is_error)
    ORDER_DIFF(is_dead, before.is_dead == after.is_dead)
#undef ORDER_DIFF
    return bits;
}

} // namespace

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
                if (o.order_id.empty()) {
                    o.order_id = oit.key();
                }
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

    const Order before = o;
    uint64_t    bits   = 0;
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
        case OrderField::advanced:
            if (auto e = try_get_enum<AdvancedOrder>(v)) {
                o.advanced = *e;
                bits |= (uint64_t)f;
            }
            break;
        case OrderField::offset_leg2:
            if (v.is_string()) {
                o.offset_leg2 = v.get<std::string>();
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
    // CTP 每次查询都会重发完整订单。不能仅因为字段存在就置 dirty；
    // 只有与合并前的值真正不同，才进入 wait_update/WS diff。
    const uint64_t actual_bits = order_diff_bits(before, o);
    if (actual_bits != 0) {
        data_.orders.mark_changed(o, actual_bits);
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
    if (bits) {
        data_.trades.mark_changed(t, bits);
    }
}

} // namespace tqsdk
