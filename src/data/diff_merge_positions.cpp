#include "tqsdk/data/diff_merge_engine.h"
#include "tqsdk/data/field_table.h"
#include "tqsdk/domain/enum_util.h"
#include "tqsdk/types.h"

namespace tqsdk {

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
            if (!v.is_number_integer() || v.get<int64_t>() < 0) {
                break;
            }
            uint64_t n = static_cast<uint64_t>(v.get<int64_t>());
            // 按字段分派到具体成员(用 f 判定)
            if (f == PositionField::volume_long) {
                p.volume_long = n;
            } else if (f == PositionField::volume_long_today) {
                p.volume_long_today = n;
            } else if (f == PositionField::volume_long_his) {
                p.volume_long_his = n;
            } else if (f == PositionField::volume_long_frozen_today) {
                p.volume_long_frozen_today = n;
            } else if (f == PositionField::volume_long_frozen_his) {
                p.volume_long_frozen_his = n;
            } else if (f == PositionField::volume_short) {
                p.volume_short = n;
            } else if (f == PositionField::volume_short_today) {
                p.volume_short_today = n;
            } else if (f == PositionField::volume_short_his) {
                p.volume_short_his = n;
            } else if (f == PositionField::volume_short_frozen_today) {
                p.volume_short_frozen_today = n;
            } else if (f == PositionField::volume_short_frozen_his) {
                p.volume_short_frozen_his = n;
            } else if (f == PositionField::order_volume_buy_open) {
                p.order_volume_buy_open = n;
            } else if (f == PositionField::order_volume_buy_close) {
                p.order_volume_buy_close = n;
            } else if (f == PositionField::order_volume_sell_open) {
                p.order_volume_sell_open = n;
            } else if (f == PositionField::order_volume_sell_close) {
                p.order_volume_sell_close = n;
            }
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
            if (!v.is_number()) {
                break;
            }
            double d = v.get<double>();
            if (f == PositionField::open_price_long) {
                p.open_price_long = d;
            } else if (f == PositionField::open_price_short) {
                p.open_price_short = d;
            } else if (f == PositionField::open_cost_long) {
                p.open_cost_long = d;
            } else if (f == PositionField::open_cost_short) {
                p.open_cost_short = d;
            } else if (f == PositionField::position_price_long) {
                p.position_price_long = d;
            } else if (f == PositionField::position_price_short) {
                p.position_price_short = d;
            } else if (f == PositionField::position_cost_long) {
                p.position_cost_long = d;
            } else if (f == PositionField::position_cost_short) {
                p.position_cost_short = d;
            } else if (f == PositionField::last_price) {
                p.last_price = d;
            } else if (f == PositionField::float_profit_long) {
                p.float_profit_long = d;
            } else if (f == PositionField::float_profit_short) {
                p.float_profit_short = d;
            } else if (f == PositionField::float_profit) {
                p.float_profit = d;
            } else if (f == PositionField::position_profit_long) {
                p.position_profit_long = d;
            } else if (f == PositionField::position_profit_short) {
                p.position_profit_short = d;
            } else if (f == PositionField::position_profit) {
                p.position_profit = d;
            } else if (f == PositionField::margin_long) {
                p.margin_long = d;
            } else if (f == PositionField::margin_short) {
                p.margin_short = d;
            } else if (f == PositionField::margin) {
                p.margin = d;
            }
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

} // namespace tqsdk
