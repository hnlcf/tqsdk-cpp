#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/domain/account.h"
#include "tqsdk/domain/order.h"
#include "tqsdk/domain/position.h"
#include "tqsdk/domain/quote.h"
#include "tqsdk/domain/trade.h"

namespace tqsdk {

consteval bool fields_sorted(auto const& arr) {
    for (std::size_t i = 1; i < arr.size(); ++i)
        if (arr[i - 1].first >= arr[i].first)
            return false;
    return true;
}

using namespace std::string_view_literals;

constexpr std::array kQuoteFields = {
    std::pair{"ask_price1"sv, QuoteField::ask_price1},
    std::pair{"ask_price2"sv, QuoteField::ask_price2},
    std::pair{"ask_price3"sv, QuoteField::ask_price3},
    std::pair{"ask_price4"sv, QuoteField::ask_price4},
    std::pair{"ask_price5"sv, QuoteField::ask_price5},
    std::pair{"ask_volume1"sv, QuoteField::ask_volume1},
    std::pair{"ask_volume2"sv, QuoteField::ask_volume2},
    std::pair{"ask_volume3"sv, QuoteField::ask_volume3},
    std::pair{"ask_volume4"sv, QuoteField::ask_volume4},
    std::pair{"ask_volume5"sv, QuoteField::ask_volume5},
    std::pair{"average"sv, QuoteField::average},
    std::pair{"bid_price1"sv, QuoteField::bid_price1},
    std::pair{"bid_price2"sv, QuoteField::bid_price2},
    std::pair{"bid_price3"sv, QuoteField::bid_price3},
    std::pair{"bid_price4"sv, QuoteField::bid_price4},
    std::pair{"bid_price5"sv, QuoteField::bid_price5},
    std::pair{"bid_volume1"sv, QuoteField::bid_volume1},
    std::pair{"bid_volume2"sv, QuoteField::bid_volume2},
    std::pair{"bid_volume3"sv, QuoteField::bid_volume3},
    std::pair{"bid_volume4"sv, QuoteField::bid_volume4},
    std::pair{"bid_volume5"sv, QuoteField::bid_volume5},
    std::pair{"close"sv, QuoteField::close},
    std::pair{"commission"sv, QuoteField::commission},
    std::pair{"datetime"sv, QuoteField::datetime},
    std::pair{"highest"sv, QuoteField::highest},
    std::pair{"last_price"sv, QuoteField::last_price},
    std::pair{"lower_limit"sv, QuoteField::lower_limit},
    std::pair{"lowest"sv, QuoteField::lowest},
    std::pair{"margin"sv, QuoteField::margin},
    std::pair{"max_limit_order_volume"sv, QuoteField::max_limit_order_volume},
    std::pair{"max_market_order_volume"sv, QuoteField::max_market_order_volume},
    std::pair{"min_limit_order_volume"sv, QuoteField::min_limit_order_volume},
    std::pair{"min_market_order_volume"sv, QuoteField::min_market_order_volume},
    std::pair{"open"sv, QuoteField::open},
    std::pair{"open_interest"sv, QuoteField::open_interest},
    std::pair{"pre_close"sv, QuoteField::pre_close},
    std::pair{"pre_open_interest"sv, QuoteField::pre_open_interest},
    std::pair{"pre_settlement"sv, QuoteField::pre_settlement},
    std::pair{"price_decs"sv, QuoteField::price_decs},
    std::pair{"price_tick"sv, QuoteField::price_tick},
    std::pair{"settlement"sv, QuoteField::settlement},
    std::pair{"turnover"sv, QuoteField::turnover},
    std::pair{"upper_limit"sv, QuoteField::upper_limit},
    std::pair{"volume"sv, QuoteField::volume},
    std::pair{"volume_multiple"sv, QuoteField::volume_multiple},
};
static_assert(fields_sorted(kQuoteFields), "kQuoteFields must be sorted");
static_assert(kQuoteFields.size() == 45, "kQuoteFields must contain 45 fields");

inline QuoteField lookup_quote_field(std::string_view name) noexcept {
    auto it = std::lower_bound(kQuoteFields.begin(),
                               kQuoteFields.end(),
                               name,
                               [](const auto& a, std::string_view b) { return a.first < b; });
    if (it == kQuoteFields.end() || it->first != name)
        return QuoteField{0};
    return it->second;
}

constexpr std::array kPositionFields = {
    std::pair{"exchange_id"sv, PositionField::exchange_id},
    std::pair{"float_profit"sv, PositionField::float_profit},
    std::pair{"float_profit_long"sv, PositionField::float_profit_long},
    std::pair{"float_profit_short"sv, PositionField::float_profit_short},
    std::pair{"last_price"sv, PositionField::last_price},
    std::pair{"margin"sv, PositionField::margin},
    std::pair{"margin_long"sv, PositionField::margin_long},
    std::pair{"margin_short"sv, PositionField::margin_short},
    std::pair{"open_cost_long"sv, PositionField::open_cost_long},
    std::pair{"open_cost_short"sv, PositionField::open_cost_short},
    std::pair{"open_price_long"sv, PositionField::open_price_long},
    std::pair{"open_price_short"sv, PositionField::open_price_short},
    std::pair{"order_volume_buy_close"sv, PositionField::order_volume_buy_close},
    std::pair{"order_volume_buy_open"sv, PositionField::order_volume_buy_open},
    std::pair{"order_volume_sell_close"sv, PositionField::order_volume_sell_close},
    std::pair{"order_volume_sell_open"sv, PositionField::order_volume_sell_open},
    std::pair{"position_cost_long"sv, PositionField::position_cost_long},
    std::pair{"position_cost_short"sv, PositionField::position_cost_short},
    std::pair{"position_price_long"sv, PositionField::position_price_long},
    std::pair{"position_price_short"sv, PositionField::position_price_short},
    std::pair{"position_profit"sv, PositionField::position_profit},
    std::pair{"position_profit_long"sv, PositionField::position_profit_long},
    std::pair{"position_profit_short"sv, PositionField::position_profit_short},
    std::pair{"volume_long"sv, PositionField::volume_long},
    std::pair{"volume_long_frozen_his"sv, PositionField::volume_long_frozen_his},
    std::pair{"volume_long_frozen_today"sv, PositionField::volume_long_frozen_today},
    std::pair{"volume_long_his"sv, PositionField::volume_long_his},
    std::pair{"volume_long_today"sv, PositionField::volume_long_today},
    std::pair{"volume_short"sv, PositionField::volume_short},
    std::pair{"volume_short_frozen_his"sv, PositionField::volume_short_frozen_his},
    std::pair{"volume_short_frozen_today"sv, PositionField::volume_short_frozen_today},
    std::pair{"volume_short_his"sv, PositionField::volume_short_his},
    std::pair{"volume_short_today"sv, PositionField::volume_short_today},
};
static_assert(fields_sorted(kPositionFields), "kPositionFields must be sorted");

inline PositionField lookup_position_field(std::string_view name) noexcept {
    auto it = std::lower_bound(kPositionFields.begin(),
                               kPositionFields.end(),
                               name,
                               [](const auto& a, std::string_view b) { return a.first < b; });
    if (it == kPositionFields.end() || it->first != name)
        return PositionField{0};
    return it->second;
}

constexpr std::array kOrderFields = {
    std::pair{"direction"sv, OrderField::direction},
    std::pair{"exchange_id"sv, OrderField::exchange_id},
    std::pair{"exchange_order_id"sv, OrderField::exchange_order_id},
    std::pair{"hedge_flag"sv, OrderField::hedge_flag},
    std::pair{"insert_date_time"sv, OrderField::insert_date_time},
    std::pair{"instrument_id"sv, OrderField::instrument_id},
    std::pair{"is_dead"sv, OrderField::is_dead},
    std::pair{"is_error"sv, OrderField::is_error},
    std::pair{"last_msg"sv, OrderField::last_msg},
    std::pair{"limit_price"sv, OrderField::limit_price},
    std::pair{"min_volume"sv, OrderField::min_volume},
    std::pair{"offset"sv, OrderField::offset},
    std::pair{"order_type"sv, OrderField::order_type},
    std::pair{"price_type"sv, OrderField::price_type},
    std::pair{"status"sv, OrderField::status},
    std::pair{"time_cond"sv, OrderField::time_cond},
    std::pair{"trade_price"sv, OrderField::trade_price},
    std::pair{"trade_type"sv, OrderField::trade_type},
    std::pair{"volume_cond"sv, OrderField::volume_cond},
    std::pair{"volume_left"sv, OrderField::volume_left},
    std::pair{"volume_orign"sv, OrderField::volume_orign},
};
static_assert(fields_sorted(kOrderFields), "kOrderFields must be sorted");

inline OrderField lookup_order_field(std::string_view name) noexcept {
    auto it = std::lower_bound(kOrderFields.begin(),
                               kOrderFields.end(),
                               name,
                               [](const auto& a, std::string_view b) { return a.first < b; });
    if (it == kOrderFields.end() || it->first != name)
        return OrderField{0};
    return it->second;
}

constexpr std::array kTradeFields = {
    std::pair{"commission"sv, TradeField::commission},
    std::pair{"direction"sv, TradeField::direction},
    std::pair{"exchange_id"sv, TradeField::exchange_id},
    std::pair{"exchange_trade_id"sv, TradeField::exchange_trade_id},
    std::pair{"instrument_id"sv, TradeField::instrument_id},
    std::pair{"offset"sv, TradeField::offset},
    std::pair{"order_id"sv, TradeField::order_id},
    std::pair{"price"sv, TradeField::price},
    std::pair{"trade_date_time"sv, TradeField::trade_date_time},
    std::pair{"volume"sv, TradeField::volume},
};
static_assert(fields_sorted(kTradeFields), "kTradeFields must be sorted");

inline TradeField lookup_trade_field(std::string_view name) noexcept {
    auto it = std::lower_bound(kTradeFields.begin(),
                               kTradeFields.end(),
                               name,
                               [](const auto& a, std::string_view b) { return a.first < b; });
    if (it == kTradeFields.end() || it->first != name)
        return TradeField{0};
    return it->second;
}

constexpr std::array kAccountFields = {
    std::pair{"available"sv, AccountField::available},
    std::pair{"balance"sv, AccountField::balance},
    std::pair{"close_profit"sv, AccountField::close_profit},
    std::pair{"commission"sv, AccountField::commission},
    std::pair{"currency"sv, AccountField::currency},
    std::pair{"deposit"sv, AccountField::deposit},
    std::pair{"float_profit"sv, AccountField::float_profit},
    std::pair{"frozen_commission"sv, AccountField::frozen_commission},
    std::pair{"frozen_margin"sv, AccountField::frozen_margin},
    std::pair{"frozen_premium"sv, AccountField::frozen_premium},
    std::pair{"margin"sv, AccountField::margin},
    std::pair{"position_profit"sv, AccountField::position_profit},
    std::pair{"pre_balance"sv, AccountField::pre_balance},
    std::pair{"premium"sv, AccountField::premium},
    std::pair{"risk_ratio"sv, AccountField::risk_ratio},
    std::pair{"static_balance"sv, AccountField::static_balance},
    std::pair{"withdraw"sv, AccountField::withdraw},
};
static_assert(fields_sorted(kAccountFields), "kAccountFields must be sorted");

inline AccountField lookup_account_field(std::string_view name) noexcept {
    auto it = std::lower_bound(kAccountFields.begin(),
                               kAccountFields.end(),
                               name,
                               [](const auto& a, std::string_view b) { return a.first < b; });
    if (it == kAccountFields.end() || it->first != name)
        return AccountField{0};
    return it->second;
}

} // namespace tqsdk
