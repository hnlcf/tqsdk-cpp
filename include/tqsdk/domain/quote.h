#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/types.h"

#include "sentinel.h"

namespace tqsdk {

enum class QuoteField : uint64_t {
    // ---- 合约静态信息 ----
    datetime                = 1ull << 0,
    volume_multiple         = 1ull << 1,
    price_tick              = 1ull << 2,
    price_decs              = 1ull << 3,
    max_market_order_volume = 1ull << 4,
    min_market_order_volume = 1ull << 5,
    max_limit_order_volume  = 1ull << 6,
    min_limit_order_volume  = 1ull << 7,
    margin                  = 1ull << 8,
    commission              = 1ull << 9,

    // ---- 价格 ----
    last_price        = 1ull << 10,
    pre_settlement    = 1ull << 11,
    pre_close         = 1ull << 12,
    pre_open_interest = 1ull << 13,
    open              = 1ull << 14,
    highest           = 1ull << 15,
    lowest            = 1ull << 16,
    volume            = 1ull << 17,
    turnover          = 1ull << 18,
    open_interest     = 1ull << 19,
    close             = 1ull << 20,
    average           = 1ull << 21,
    settlement        = 1ull << 22,
    upper_limit       = 1ull << 23,
    lower_limit       = 1ull << 24,

    // ---- 五档盘口价格 ----
    bid_price1 = 1ull << 25,
    ask_price1 = 1ull << 26,
    bid_price2 = 1ull << 27,
    ask_price2 = 1ull << 28,
    bid_price3 = 1ull << 29,
    ask_price3 = 1ull << 30,
    bid_price4 = 1ull << 31,
    ask_price4 = 1ull << 32,
    bid_price5 = 1ull << 33,
    ask_price5 = 1ull << 34,

    // ---- 五档盘口量 ----
    bid_volume1 = 1ull << 35,
    ask_volume1 = 1ull << 36,
    bid_volume2 = 1ull << 37,
    ask_volume2 = 1ull << 38,
    bid_volume3 = 1ull << 39,
    ask_volume3 = 1ull << 40,
    bid_volume4 = 1ull << 41,
    ask_volume4 = 1ull << 42,
    bid_volume5 = 1ull << 43,
    ask_volume5 = 1ull << 44,
};

// 编译期保证全部字段位不越界
static_assert(static_cast<uint64_t>(QuoteField::ask_volume5) < (1ull << 63));

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
