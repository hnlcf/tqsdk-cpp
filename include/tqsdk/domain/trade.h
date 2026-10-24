#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/types.h"

#include "sentinel.h"

namespace tqsdk {

enum class TradeField : uint64_t {
    // TODO
};

struct Trade {
    OrderId order_id;

    /// 委托单代码
    TradeId trade_id;

    /// 交易所代码
    ExchangeId exchange_id;

    /// 合约代码
    InstrumentId instrument_id;

    /// 交易所单号
    ExchangeTradeId exchange_trade_id;

    /// 成交方向
    Direction direction = Direction::BUY;

    /// 开平标志
    Offset offset = Offset::Open;

    /// 成交手数
    uint64_t volume = 0;

    /// 成交价格
    double price = kUnknownDouble;

    /// 成交时间
    TimeStamp trade_date_time;

    /// 交易手续费
    double commission;

    /// 字段更新标志位图
    uint64_t changed_fields = 0;
};

} // namespace tqsdk
