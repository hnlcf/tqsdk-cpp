#include "tqsdk/data/diff_merge_engine.h"
#include "tqsdk/data/field_table.h"
#include "tqsdk/domain/enum_util.h"
#include "tqsdk/types.h"

namespace tqsdk {

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
    if (bits) {
        data_.quotes.mark_changed(q, bits);
    }
}

} // namespace tqsdk
