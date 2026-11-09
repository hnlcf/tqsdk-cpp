#pragma once

#include "tqsdk/domain/quote.h"
#include "tqsdk/gateway/trading_gateway.h"

#include <cmath>
#include <string>

namespace tqsdk {

/// SHFE 不支持 CTP 的 AnyPrice/BestPrice 报单类型。这里把用户的
/// “市价/对价”意图翻译成实际可报的限价单：
///   - 市价 BUY  -> 涨停价限价，IOC；
///   - 市价 SELL -> 跌停价限价，IOC；
///   - 对价 BUY  -> 卖一价限价，IOC；
///   - 对价 SELL -> 买一价限价，IOC。
/// 其他交易所/其他价格类型原样返回。
struct OrderPriceNormalization {
    InsertOrderReq request;
    bool           converted = false;
    std::string    error;

    bool ok() const noexcept { return error.empty(); }
};

/// 将官方 advanced=FAK/FOK 与 CTP 的 TimeCondition/VolumeCondition 对齐。
/// CTP 只有 IOC，FAK/FOK 的差别在数量条件：ANY / ALL。
inline InsertOrderReq apply_advanced_order(const InsertOrderReq& request) {
    InsertOrderReq result = request;
    if (result.advanced == AdvancedOrder::FAK) {
        result.time_cond   = TimeCondition::IOC;
        result.volume_cond = VolumeCondition::ANY;
    } else if (result.advanced == AdvancedOrder::FOK) {
        result.time_cond   = TimeCondition::IOC;
        result.volume_cond = VolumeCondition::ALL;
    } else if (result.price_type != PriceType::LIMIT && result.time_cond == TimeCondition::GFD) {
        // 市价/最优价/五档价默认是立即成交，避免 GFD 挂单。
        result.time_cond = TimeCondition::IOC;
    }
    return result;
}

inline std::string order_exchange(const InsertOrderReq& request) {
    if (!request.exchange_id.empty()) {
        return request.exchange_id;
    }
    const auto dot = request.instrument_id.find('.');
    if (dot == std::string::npos) {
        return {};
    }
    return request.instrument_id.substr(0, dot);
}

inline bool needs_shfe_limit_conversion(const InsertOrderReq& request) {
    return order_exchange(request) == "SHFE" &&
           (request.price_type == PriceType::ANY || request.price_type == PriceType::BEST);
}

inline OrderPriceNormalization normalize_shfe_order_price(const InsertOrderReq& request,
                                                          const Quote*          quote) {
    OrderPriceNormalization result{request, false, {}};
    if (!needs_shfe_limit_conversion(request)) {
        return result;
    }
    if (quote == nullptr) {
        result.error = "SHFE 市价/对价单缺少行情快照";
        return result;
    }

    double price = kUnknownDouble;
    if (request.price_type == PriceType::ANY) {
        price = request.direction == Direction::BUY ? quote->upper_limit : quote->lower_limit;
    } else { // BEST / 对价
        price = request.direction == Direction::BUY ? quote->ask_price1 : quote->bid_price1;
    }
    if (!std::isfinite(price) || price <= 0.0) {
        result.error = request.price_type == PriceType::ANY ? "SHFE 市价单缺少有效停板价"
                                                            : "SHFE 对价单缺少有效买一/卖一价";
        return result;
    }

    result.request.price_type  = PriceType::LIMIT;
    result.request.limit_price = price;
    // 市价/对价语义是立即成交，不留下挂单。
    result.request.time_cond = TimeCondition::IOC;
    result.converted         = true;
    return result;
}

} // namespace tqsdk
