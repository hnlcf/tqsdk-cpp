#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/util/serde.h"

namespace tqsdk {

using Symbol       = std::string;
using InstrumentId = std::string;
using ExchangeId   = std::string;

using OrderId         = std::string;
using ExchangeOrderId = std::string;

using TradeId         = std::string;
using ExchangeTradeId = std::string;

using AccountId = std::string;

/// Unix epoch nano seconds, like `1928374000000000`
using TimeStamp = uint64_t;

/// Datetime string, like `"2016-12-30 13:21:32.500000"`
using Datetime = std::string;

enum class Currency : uint8_t {
    CNY,
    USD,
    HKD,
};
DEFINE_ENUM_JSON_SERIALIZE(Currency)

enum class OrderStatus : uint8_t {
    ALIVE,
    FINISHED
};
DEFINE_ENUM_JSON_SERIALIZE(OrderStatus)

enum class OrderType : uint8_t {
    /// 交易指令
    TRADE,
    /// 互换交易指令
    SWAP,
    /// 期权行权指令
    EXECUTE,
    /// 期权询价指令
    QUOTE
};
DEFINE_ENUM_JSON_SERIALIZE(OrderType)

enum class Direction : uint8_t {
    BUY,
    SELL
};
DEFINE_ENUM_JSON_SERIALIZE(Direction)

enum class Offset : uint8_t {
    OPEN,
    CLOSE,
    CLOSETODAY,
    CLOSEYESTERDAY,
};
DEFINE_ENUM_JSON_SERIALIZE(Offset)

enum class TradeType {
    /// 止损
    STOPLOSS,
    /// 止盈
    TAKEPROFIT,
};
DEFINE_ENUM_JSON_SERIALIZE(TradeType)

enum class PriceType {
    /// 任意价
    ANY,
    /// 限价
    LIMIT,
    /// 最优价
    BEST,
    /// 五档价
    FIVELEVEL,
};
DEFINE_ENUM_JSON_SERIALIZE(PriceType)

enum class VolumeCondition : uint8_t {
    /// 任何数量
    ANY,
    /// 最小数量
    MIN,
    /// 全部数量
    ALL
};
DEFINE_ENUM_JSON_SERIALIZE(VolumeCondition)

enum class TimeCondition : uint8_t {
    /// 立即完成，否则撤销
    IOC,
    /// 本节有效
    GFS,
    /// 当日有效
    GFD,
    /// 指定日期前有效
    GTD,
    /// 撤销前有效
    GTC,
    /// 集合竞价有效
    GFA,
    FOK
};
DEFINE_ENUM_JSON_SERIALIZE(TimeCondition)

enum class ForceClose : uint8_t {
    /// 非强平
    NOT,
    /// 资金不足
    LACK_DEPOSIT,
    /// 客户超仓
    CLIENT_POSITION_LIMIT,
    /// 会员超仓
    MEMBER_POSITION_LIMIT,
    /// 持仓非整数倍
    POSITION_MULTIPLE,
    /// 违规
    VIOLATION,
    /// 其他
    OTHER,
    /// 自然人临近交割
    PERSONAL_DELIV,
    /// 客户套保超仓
    HEDGE_POSITION_LIMIT,
};
DEFINE_ENUM_JSON_SERIALIZE(ForceClose)

} // namespace tqsdk
