#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/domain/enum_util.h"
#include "tqsdk/domain/sentinel.h"
#include "tqsdk/types.h"

#include <ThostFtdcUserApiDataType.h>
#include <ThostFtdcUserApiStruct.h>

#include <cmath>
#include <cstring>

namespace tqsdk {
namespace ctp {

inline std::string cstr(const char* p) {
    return p == nullptr ? std::string{} : std::string{p};
}

inline double finite_price(double v) {
    // CTP 用 DBL_MAX 表示无效价格。
    return (std::isfinite(v) && v < 1e100) ? v : kUnknownDouble;
}

inline void set_field(char* dst, std::size_t n, std::string_view value) {
    if (dst == nullptr || n == 0) {
        return;
    }
    std::memset(dst, 0, n);
    const std::size_t len = std::min(n - 1, value.size());
    std::memcpy(dst, value.data(), len);
}

inline TimeStamp ctp_time(std::string_view date, std::string_view time, int millisec = 0) {
    if (date.size() != 8 || time.size() < 8) {
        return 0;
    }
    std::tm tm{};
    try {
        tm.tm_year = std::stoi(std::string{date.substr(0, 4)}) - 1900;
        tm.tm_mon  = std::stoi(std::string{date.substr(4, 2)}) - 1;
        tm.tm_mday = std::stoi(std::string{date.substr(6, 2)});
        tm.tm_hour = std::stoi(std::string{time.substr(0, 2)});
        tm.tm_min  = std::stoi(std::string{time.substr(3, 2)});
        tm.tm_sec  = std::stoi(std::string{time.substr(6, 2)});
    } catch (...) {
        return 0;
    }
    const std::time_t sec = std::mktime(&tm);
    if (sec < 0) {
        return 0;
    }
    return static_cast<TimeStamp>(sec) * 1'000'000'000ULL +
           static_cast<TimeStamp>(std::max(0, millisec)) * 1'000'000ULL;
}

inline TimeStamp ctp_order_time(const CThostFtdcOrderField& o) {
    auto ts = ctp_time(cstr(o.InsertDate), cstr(o.InsertTime));
    if (ts == 0) {
        ts = ctp_time(cstr(o.TradingDay), cstr(o.InsertTime));
    }
    return ts;
}

inline TimeStamp ctp_trade_time(const CThostFtdcTradeField& t) {
    auto ts = ctp_time(cstr(t.TradeDate), cstr(t.TradeTime));
    if (ts == 0) {
        ts = ctp_time(cstr(t.TradingDay), cstr(t.TradeTime));
    }
    return ts;
}

inline char ctp_direction(Direction d) {
    return d == Direction::SELL ? THOST_FTDC_D_Sell : THOST_FTDC_D_Buy;
}

inline char ctp_offset(Offset o) {
    switch (o) {
    case Offset::CLOSE:
        return THOST_FTDC_OF_Close;
    case Offset::CLOSETODAY:
        return THOST_FTDC_OF_CloseToday;
    case Offset::CLOSEYESTERDAY:
        return THOST_FTDC_OF_CloseYesterday;
    default:
        return THOST_FTDC_OF_Open;
    }
}

inline char ctp_price_type(PriceType p) {
    switch (p) {
    case PriceType::ANY:
        return THOST_FTDC_OPT_AnyPrice;
    case PriceType::BEST:
        return THOST_FTDC_OPT_BestPrice;
    case PriceType::FIVELEVEL:
        return THOST_FTDC_OPT_FiveLevelPrice;
    default:
        return THOST_FTDC_OPT_LimitPrice;
    }
}

inline char ctp_time_cond(TimeCondition t) {
    switch (t) {
    case TimeCondition::IOC:
    case TimeCondition::FOK:
        return THOST_FTDC_TC_IOC;
    case TimeCondition::GFS:
        return THOST_FTDC_TC_GFS;
    case TimeCondition::GTD:
        return THOST_FTDC_TC_GTD;
    case TimeCondition::GTC:
        return THOST_FTDC_TC_GTC;
    case TimeCondition::GFA:
        return THOST_FTDC_TC_GFA;
    default:
        return THOST_FTDC_TC_GFD;
    }
}

inline char ctp_volume_cond(VolumeCondition v) {
    switch (v) {
    case VolumeCondition::MIN:
        return THOST_FTDC_VC_MV;
    case VolumeCondition::ALL:
        return THOST_FTDC_VC_CV;
    default:
        return THOST_FTDC_VC_AV;
    }
}

inline const char* direction_str(char c) {
    return c == THOST_FTDC_D_Sell ? "SELL" : "BUY";
}

inline const char* offset_str(char c) {
    switch (c) {
    case THOST_FTDC_OF_Close:
        return "CLOSE";
    case THOST_FTDC_OF_CloseToday:
        return "CLOSETODAY";
    case THOST_FTDC_OF_CloseYesterday:
        return "CLOSEYESTERDAY";
    default:
        return "OPEN";
    }
}

inline const char* price_type_str(char c) {
    switch (c) {
    case THOST_FTDC_OPT_AnyPrice:
        return "ANY";
    case THOST_FTDC_OPT_BestPrice:
        return "BEST";
    case THOST_FTDC_OPT_FiveLevelPrice:
        return "FIVELEVEL";
    default:
        return "LIMIT";
    }
}

inline const char* time_cond_str(char c) {
    switch (c) {
    case THOST_FTDC_TC_IOC:
        return "IOC";
    case THOST_FTDC_TC_GFS:
        return "GFS";
    case THOST_FTDC_TC_GTD:
        return "GTD";
    case THOST_FTDC_TC_GTC:
        return "GTC";
    case THOST_FTDC_TC_GFA:
        return "GFA";
    default:
        return "GFD";
    }
}

inline const char* volume_cond_str(char c) {
    switch (c) {
    case THOST_FTDC_VC_MV:
        return "MIN";
    case THOST_FTDC_VC_CV:
        return "ALL";
    default:
        return "ANY";
    }
}

inline bool ctp_order_finished(char status) {
    switch (status) {
    case THOST_FTDC_OST_AllTraded:
    case THOST_FTDC_OST_Canceled:
    case THOST_FTDC_OST_PartTradedNotQueueing:
    case THOST_FTDC_OST_NoTradeNotQueueing:
        return true;
    default:
        return false;
    }
}

inline nlohmann::json order_json(const CThostFtdcOrderField& o, std::string_view rid) {
    const bool finished = ctp_order_finished(o.OrderStatus);
    const auto offset   = o.CombOffsetFlag[0];
    return nlohmann::json{
        {"order_id", std::string{rid}},
        {"exchange_order_id", cstr(o.OrderSysID)},
        {"exchange_id", cstr(o.ExchangeID)},
        {"instrument_id", cstr(o.InstrumentID)},
        {"direction", direction_str(o.Direction)},
        {"offset", offset_str(offset)},
        {"volume_orign", o.VolumeTotalOriginal},
        {"volume_left", o.VolumeTotal},
        {"price_type", price_type_str(o.OrderPriceType)},
        {"limit_price",
         o.OrderPriceType == THOST_FTDC_OPT_LimitPrice ? finite_price(o.LimitPrice)
                                                       : kUnknownDouble},
        {"time_cond", time_cond_str(o.TimeCondition)},
        {"volume_cond", volume_cond_str(o.VolumeCondition)},
        {"min_volume", o.MinVolume},
        {"status", finished ? "FINISHED" : "ALIVE"},
        {"last_msg", cstr(o.StatusMsg)},
        {"insert_date_time", ctp_order_time(o)},
        {"is_dead", finished},
        {"is_error", o.OrderSubmitStatus == THOST_FTDC_OSS_InsertRejected},
    };
}

inline nlohmann::json trade_json(const CThostFtdcTradeField& t, std::string_view rid) {
    return nlohmann::json{
        {"order_id", std::string{rid}},
        {"trade_id", cstr(t.TradeID)},
        {"exchange_trade_id", cstr(t.TradeID)},
        {"exchange_id", cstr(t.ExchangeID)},
        {"instrument_id", cstr(t.InstrumentID)},
        {"direction", direction_str(t.Direction)},
        {"offset", offset_str(t.OffsetFlag)},
        {"volume", t.Volume},
        {"price", finite_price(t.Price)},
        {"trade_date_time", ctp_trade_time(t)},
        {"commission", kUnknownDouble},
    };
}

struct InstrumentSpec {
    double volume_multiple = 1.0;
    double price_tick      = 0.0;
    double long_margin     = 0.0;
    double short_margin    = 0.0;
};

inline InstrumentSpec parse_instrument(const CThostFtdcInstrumentField& i) {
    InstrumentSpec s;
    s.volume_multiple = i.VolumeMultiple > 0 ? static_cast<double>(i.VolumeMultiple) : 1.0;
    s.price_tick      = finite_price(i.PriceTick);
    s.long_margin     = i.LongMarginRatio;
    s.short_margin    = i.ShortMarginRatio;
    return s;
}

struct PositionAgg {
    uint64_t volume_long        = 0;
    uint64_t volume_short       = 0;
    uint64_t volume_long_today  = 0;
    uint64_t volume_short_today = 0;
    double   cost_long          = 0.0;
    double   cost_short         = 0.0;
    double   margin_long        = 0.0;
    double   margin_short       = 0.0;
    double   profit_long        = 0.0;
    double   profit_short       = 0.0;
};

inline void add_position(PositionAgg&                           agg,
                         const CThostFtdcInvestorPositionField& p,
                         const InstrumentSpec&                  spec) {
    const double m = spec.volume_multiple > 0 ? spec.volume_multiple : 1.0;
    (void)m; // CTP PositionCost/OpenCost 已是金额，不再乘合约乘数。
    if (p.PosiDirection == THOST_FTDC_PD_Long) {
        agg.volume_long += static_cast<uint64_t>(std::max(0, p.Position));
        agg.volume_long_today += static_cast<uint64_t>(std::max(0, p.TodayPosition));
        agg.cost_long += p.OpenCost;
        agg.margin_long += p.UseMargin;
        agg.profit_long += p.PositionProfit;
    } else if (p.PosiDirection == THOST_FTDC_PD_Short) {
        agg.volume_short += static_cast<uint64_t>(std::max(0, p.Position));
        agg.volume_short_today += static_cast<uint64_t>(std::max(0, p.TodayPosition));
        agg.cost_short += p.OpenCost;
        agg.margin_short += p.UseMargin;
        agg.profit_short += p.PositionProfit;
    }
}

inline nlohmann::json
position_json(std::string_view symbol, const PositionAgg& p, const InstrumentSpec& spec) {
    const double m = spec.volume_multiple > 0 ? spec.volume_multiple : 1.0;
    const double open_long =
        p.volume_long > 0 ? p.cost_long / (static_cast<double>(p.volume_long) * m) : kUnknownDouble;
    const double open_short = p.volume_short > 0
                                  ? p.cost_short / (static_cast<double>(p.volume_short) * m)
                                  : kUnknownDouble;
    return nlohmann::json{
        {"instrument_id", std::string{symbol}},
        {"volume_long", p.volume_long},
        {"volume_short", p.volume_short},
        {"volume_long_today", p.volume_long_today},
        {"volume_short_today", p.volume_short_today},
        {"open_price_long", open_long},
        {"open_price_short", open_short},
        {"position_price_long", open_long},
        {"position_price_short", open_short},
        {"float_profit_long", p.profit_long},
        {"float_profit_short", p.profit_short},
        {"float_profit", p.profit_long + p.profit_short},
        {"margin_long", p.margin_long},
        {"margin_short", p.margin_short},
        {"margin", p.margin_long + p.margin_short},
    };
}

inline nlohmann::json account_json(const CThostFtdcTradingAccountField& a) {
    const double risk = a.Balance > 0 ? 1.0 - a.Available / a.Balance : 0.0;
    return nlohmann::json{
        {"balance", a.Balance},
        {"pre_balance", a.PreBalance},
        {"close_profit", a.CloseProfit},
        {"commission", a.Commission},
        {"margin", a.CurrMargin},
        {"frozen_margin", a.FrozenMargin},
        {"frozen_commission", a.FrozenCommission},
        {"available", a.Available},
        {"risk_ratio", risk},
        {"deposit", a.Deposit},
        {"withdraw", a.Withdraw},
        {"premium", a.Credit},
        {"static_balance", a.Balance},
        {"float_profit", a.PositionProfit},
    };
}

inline nlohmann::json user_diff(nlohmann::json orders,
                                nlohmann::json trades,
                                nlohmann::json positions,
                                nlohmann::json account) {
    nlohmann::json user = nlohmann::json::object();
    if (!orders.empty()) {
        user["orders"] = std::move(orders);
    }
    if (!trades.empty()) {
        user["trades"] = std::move(trades);
    }
    if (!positions.empty()) {
        user["positions"] = std::move(positions);
    }
    if (!account.empty()) {
        user["accounts"] = {{"CNY", std::move(account)}};
    }
    return nlohmann::json{{"trade", {{"", {{"user", std::move(user)}}}}}};
}

inline nlohmann::json depth_json(const CThostFtdcDepthMarketDataField& d) {
    auto px = [](double v) { return finite_price(v); };
    return nlohmann::json{
        {"instrument_id", cstr(d.InstrumentID)},
        {"datetime", cstr(d.UpdateTime)},
        {"last_price", px(d.LastPrice)},
        {"pre_close", px(d.PreClosePrice)},
        {"pre_settlement", px(d.PreSettlementPrice)},
        {"open", px(d.OpenPrice)},
        {"highest", px(d.HighestPrice)},
        {"lowest", px(d.LowestPrice)},
        {"close", px(d.ClosePrice)},
        {"settlement", px(d.SettlementPrice)},
        {"upper_limit", px(d.UpperLimitPrice)},
        {"lower_limit", px(d.LowerLimitPrice)},
        {"volume", d.Volume},
        {"turnover", d.Turnover},
        {"open_interest", static_cast<double>(d.OpenInterest)},
        {"pre_open_interest", static_cast<double>(d.PreOpenInterest)},
        {"bid_price1", px(d.BidPrice1)},
        {"bid_volume1", d.BidVolume1},
        {"ask_price1", px(d.AskPrice1)},
        {"ask_volume1", d.AskVolume1},
        {"bid_price2", px(d.BidPrice2)},
        {"bid_volume2", d.BidVolume2},
        {"ask_price2", px(d.AskPrice2)},
        {"ask_volume2", d.AskVolume2},
        {"bid_price3", px(d.BidPrice3)},
        {"bid_volume3", d.BidVolume3},
        {"ask_price3", px(d.AskPrice3)},
        {"ask_volume3", d.AskVolume3},
        {"bid_price4", px(d.BidPrice4)},
        {"bid_volume4", d.BidVolume4},
        {"ask_price4", px(d.AskPrice4)},
        {"ask_volume4", d.AskVolume4},
        {"bid_price5", px(d.BidPrice5)},
        {"bid_volume5", d.BidVolume5},
        {"ask_price5", px(d.AskPrice5)},
        {"ask_volume5", d.AskVolume5},
    };
}

} // namespace ctp
} // namespace tqsdk
