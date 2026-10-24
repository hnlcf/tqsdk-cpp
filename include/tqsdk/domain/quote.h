#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/types.h"

#include "sentinel.h"

namespace tqsdk {

// Quote fields
enum class QuoteField : uint64_t {
    datetime          = 1ull << 0,
    last_price        = 1ull << 1,
    pre_settlement    = 1ull << 2,
    pre_close         = 1ull << 3,
    pre_open_interest = 1ull << 4,
    open              = 1ull << 5,
    highest           = 1ull << 6,
    lowest            = 1ull << 7,
    volume            = 1ull << 8,
    turnover          = 1ull << 9,
    open_interest     = 1ull << 10,
    close             = 1ull << 11,
    average           = 1ull << 12,
    settlement        = 1ull << 13,
    upper_limit       = 1ull << 14,
    lower_limit       = 1ull << 15,
    bid_price1        = 1ull << 16,
    ask_price1        = 1ull << 17,
    bid_price2        = 1ull << 18,
    ask_price2        = 1ull << 19,
    bid_price3        = 1ull << 20,
    ask_price3        = 1ull << 21,
    bid_price4        = 1ull << 22,
    ask_price4        = 1ull << 23,
    bid_price5        = 1ull << 24,
    ask_price5        = 1ull << 25,
    bid_volume1       = 1ull << 26,
    ask_volume1       = 1ull << 27,
    bid_volume2       = 1ull << 28,
    ask_volume2       = 1ull << 29,
    bid_volume3       = 1ull << 30,
    ask_volume3       = 1ull << 31,
    bid_volume4       = 1ull << 32,
    ask_volume4       = 1ull << 33,
    bid_volume5       = 1ull << 34,
    ask_volume5       = 1ull << 35,
};

struct Quote {
    /// 时间
    Datetime datetime;

    /// 合约代码
    InstrumentId instrument_id;

    /// 合约乘数
    uint64_t volume_multiple;

    /// 合约价格单位
    double price_tick;

    /// 合约价格小数位数
    uint64_t price_decs;

    /// 市价单最大下单手数
    uint64_t max_market_order_volume;

    /// 市价单最小下单手数
    uint64_t min_market_order_volume;

    /// 限价单最大下单手数
    uint64_t max_limit_order_volume;

    /// 限价单最小下单手数
    uint64_t min_limit_order_volume;

    /// 每手保证金
    double margin;

    /// 每手手续费
    double commission;

    /// 最新价
    double last_price = kUnknownDouble;

    /// 昨结算价
    double pre_settlement = kUnknownDouble;

    /// 昨收盘价
    double pre_close = kUnknownDouble;

    /// 昨持仓量
    double pre_open_interest = kUnknownDouble;

    /// 开盘价
    double open = kUnknownDouble;

    /// 最高价
    double highest = kUnknownDouble;

    /// 最低价
    double lowest = kUnknownDouble;

    /// 成交量
    uint64_t volume = 0;

    /// 成交额
    double turnover = kUnknownDouble;

    /// 持仓量
    double open_interest = kUnknownDouble;

    /// 收盘价
    double close = kUnknownDouble;

    /// 均价
    double average = kUnknownDouble;

    /// 结算价
    double settlement = kUnknownDouble;

    /// 涨停价
    double upper_limit = kUnknownDouble;

    /// 跌停价
    double lower_limit = kUnknownDouble;

    double   bid_price1  = kUnknownDouble;
    double   ask_price1  = kUnknownDouble;
    double   bid_price2  = kUnknownDouble;
    double   ask_price2  = kUnknownDouble;
    double   bid_price3  = kUnknownDouble;
    double   ask_price3  = kUnknownDouble;
    double   bid_price4  = kUnknownDouble;
    double   ask_price4  = kUnknownDouble;
    double   bid_price5  = kUnknownDouble;
    double   ask_price5  = kUnknownDouble;
    uint64_t bid_volume1 = 0;
    uint64_t ask_volume1 = 0;
    uint64_t bid_volume2 = 0;
    uint64_t ask_volume2 = 0;
    uint64_t bid_volume3 = 0;
    uint64_t ask_volume3 = 0;
    uint64_t bid_volume4 = 0;
    uint64_t ask_volume4 = 0;
    uint64_t bid_volume5 = 0;
    uint64_t ask_volume5 = 0;

    /// 字段更新标志位图
    uint64_t changed_fields = 0;
};

} // namespace tqsdk
